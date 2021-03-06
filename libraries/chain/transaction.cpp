/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <algorithm>
#include <fc/bitutil.hpp>
#include <fc/io/raw.hpp>
#include <fc/smart_ref_impl.hpp>

#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <evt/chain/exceptions.hpp>
#include <evt/chain/transaction.hpp>

namespace evt { namespace chain {

using namespace boost::multi_index;

struct cached_pub_key {
    transaction_id_type trx_id;
    public_key_type     pub_key;
    signature_type      sig;
    cached_pub_key(const cached_pub_key&) = delete;
    cached_pub_key()                      = delete;
    cached_pub_key& operator=(const cached_pub_key&) = delete;
    cached_pub_key(cached_pub_key&&)                 = default;
};
struct by_sig {};

typedef multi_index_container<
    cached_pub_key,
    indexed_by<sequenced<>, hashed_unique<tag<by_sig>, member<cached_pub_key, signature_type, &cached_pub_key::sig>>>>
    recovery_cache_type;

void
transaction_header::set_reference_block(const block_id_type& reference_block) {
    ref_block_num    = fc::endian_reverse_u32(reference_block._hash[0]);
    ref_block_prefix = reference_block._hash[1];
}

bool
transaction_header::verify_reference_block(const block_id_type& reference_block) const {
    return ref_block_num == (decltype(ref_block_num))fc::endian_reverse_u32(reference_block._hash[0])
           && ref_block_prefix == (decltype(ref_block_prefix))reference_block._hash[1];
}

void
transaction_header::validate() const {}

transaction_id_type
transaction::id() const {
    digest_type::encoder enc;
    fc::raw::pack(enc, *this);
    return enc.result();
}

digest_type
transaction::sig_digest(const chain_id_type& chain_id) const {
    digest_type::encoder enc;
    fc::raw::pack(enc, chain_id);
    fc::raw::pack(enc, *this);
    return enc.result();
}

flat_set<public_key_type>
transaction::get_signature_keys(const vector<signature_type>& signatures, const chain_id_type& chain_id,
                                bool allow_duplicate_keys) const {
    try {
        using boost::adaptors::transformed;

        constexpr size_t           recovery_cache_size = 1000;
        static recovery_cache_type recovery_cache;
        const digest_type          digest = sig_digest(chain_id);

        flat_set<public_key_type> recovered_pub_keys;
        for(const signature_type& sig : signatures) {
            recovery_cache_type::index<by_sig>::type::iterator it = recovery_cache.get<by_sig>().find(sig);

            public_key_type recov;
            if(it == recovery_cache.get<by_sig>().end() || it->trx_id != id()) {
                recov = public_key_type(sig, digest);
                recovery_cache.emplace_back(
                    cached_pub_key{id(), recov, sig});  // could fail on dup signatures; not a problem
            }
            else {
                recov = it->pub_key;
            }
            bool successful_insertion                   = false;
            std::tie(std::ignore, successful_insertion) = recovered_pub_keys.insert(recov);
            EVT_ASSERT(allow_duplicate_keys || successful_insertion, tx_duplicate_sig,
                       "transaction includes more than one signature signed using the same key associated with public "
                       "key: ${key}",
                       ("key", recov));
        }

        while(recovery_cache.size() > recovery_cache_size)
            recovery_cache.erase(recovery_cache.begin());

        return recovered_pub_keys;
    }
    FC_CAPTURE_AND_RETHROW()
}

const signature_type&
signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id) {
    signatures.push_back(key.sign(sig_digest(chain_id)));
    return signatures.back();
}

signature_type
signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id) const {
    return key.sign(sig_digest(chain_id));
}

flat_set<public_key_type>
signed_transaction::get_signature_keys(const chain_id_type& chain_id, bool allow_duplicate_keys) const {
    return transaction::get_signature_keys(signatures, chain_id, allow_duplicate_keys);
}

uint32_t
packed_transaction::get_unprunable_size() const {
    uint64_t size = config::fixed_net_overhead_of_packed_trx;
    size += packed_trx.size();
    FC_ASSERT(size <= std::numeric_limits<uint32_t>::max(), "packed_transaction is too big");
    return static_cast<uint32_t>(size);
}

uint32_t
packed_transaction::get_prunable_size() const {
    uint64_t size = fc::raw::pack_size(signatures);
    FC_ASSERT(size <= std::numeric_limits<uint32_t>::max(), "packed_transaction is too big");
    return static_cast<uint32_t>(size);
}

digest_type
packed_transaction::packed_digest() const {
    digest_type::encoder prunable;
    fc::raw::pack(prunable, signatures);

    digest_type::encoder enc;
    fc::raw::pack(enc, compression);
    fc::raw::pack(enc, packed_trx);
    fc::raw::pack(enc, prunable.result());

    return enc.result();
}

namespace bio = boost::iostreams;

template <size_t Limit>
struct read_limiter {
    using char_type = char;
    using category  = bio::multichar_output_filter_tag;

    template <typename Sink>
    size_t
    write(Sink& sink, const char* s, size_t count) {
        EVT_ASSERT(_total + count <= Limit, tx_decompression_error, "Exceeded maximum decompressed transaction size");
        _total += count;
        return bio::write(sink, s, count);
    }

    size_t _total = 0;
};

static transaction
unpack_transaction(const bytes& data) {
    return fc::raw::unpack<transaction>(data);
}

static bytes
zlib_decompress(const bytes& data) {
    try {
        bytes                  out;
        bio::filtering_ostream decomp;
        decomp.push(bio::zlib_decompressor());
        decomp.push(read_limiter<1 * 1024 * 1024>());  // limit to 10 megs decompressed for zip bomb protections
        decomp.push(bio::back_inserter(out));
        bio::write(decomp, data.data(), data.size());
        bio::close(decomp);
        return out;
    }
    catch(fc::exception& er) {
        throw;
    }
    catch(...) {
        fc::unhandled_exception er(FC_LOG_MESSAGE(warn, "internal decompression error"), std::current_exception());
        throw er;
    }
}

static transaction
zlib_decompress_transaction(const bytes& data) {
    bytes out = zlib_decompress(data);
    return unpack_transaction(out);
}

static bytes
pack_transaction(const transaction& t) {
    return fc::raw::pack(t);
}

static bytes
zlib_compress_transaction(const transaction& t) {
    bytes                  in = pack_transaction(t);
    bytes                  out;
    bio::filtering_ostream comp;
    comp.push(bio::zlib_compressor(bio::zlib::best_compression));
    comp.push(bio::back_inserter(out));
    bio::write(comp, in.data(), in.size());
    bio::close(comp);
    return out;
}

bytes
packed_transaction::get_raw_transaction() const {
    try {
        switch(compression) {
        case none:
            return packed_trx;
        case zlib:
            return zlib_decompress(packed_trx);
        default:
            FC_THROW("Unknown transaction compression algorithm");
        }
    }
    FC_CAPTURE_AND_RETHROW((compression)(packed_trx))
}

time_point_sec
packed_transaction::expiration() const {
    local_unpack();
    return unpacked_trx->expiration;
}

transaction_id_type
packed_transaction::id() const {
    local_unpack();
    return get_transaction().id();
}

void
packed_transaction::local_unpack() const {
    if(!unpacked_trx) {
        try {
            switch(compression) {
            case none:
                unpacked_trx = unpack_transaction(packed_trx);
                break;
            case zlib:
                unpacked_trx = zlib_decompress_transaction(packed_trx);
                break;
            default:
                FC_THROW("Unknown transaction compression algorithm");
            }
        }
        FC_CAPTURE_AND_RETHROW((compression)(packed_trx))
    }
}

transaction
packed_transaction::get_transaction() const {
    local_unpack();
    return transaction(*unpacked_trx);
}

signed_transaction
packed_transaction::get_signed_transaction() const {
    try {
        switch(compression) {
        case none:
            return signed_transaction(get_transaction(), signatures);
        case zlib:
            return signed_transaction(get_transaction(), signatures);
        default:
            FC_THROW("Unknown transaction compression algorithm");
        }
    }
    FC_CAPTURE_AND_RETHROW((compression)(packed_trx))
}

void
packed_transaction::set_transaction(const transaction& t, packed_transaction::compression_type _compression) {
    try {
        switch(_compression) {
        case none:
            packed_trx = pack_transaction(t);
            break;
        case zlib:
            packed_trx = zlib_compress_transaction(t);
            break;
        default:
            FC_THROW("Unknown transaction compression algorithm");
        }
    }
    FC_CAPTURE_AND_RETHROW((_compression)(t))
    compression = _compression;
}

}}  // namespace evt::chain
