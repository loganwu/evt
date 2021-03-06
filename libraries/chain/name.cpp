/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <boost/algorithm/string.hpp>
#include <evt/chain/exceptions.hpp>
#include <evt/chain/name.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>

namespace evt { namespace chain {

void
name::set(const char* str) {
    const auto len = strnlen(str, 14);
    EVT_ASSERT(len <= 13, name_type_exception, "Name is longer than 13 characters (${name}) ", ("name", string(str)));
    value = string_to_name(str);
    EVT_ASSERT(to_string() == string(str), name_type_exception,
               "Name not properly normalized (name: ${name}, normalized: ${normalized}) ",
               ("name", string(str))("normalized", to_string()));
}

void
name128::set(const char* str) {
    const auto len = strnlen(str, 22);
    EVT_ASSERT(len <= 21, name_type_exception, "Name128 is longer than 21 characters (${name}) ",
               ("name", string(str)));
    value = string_to_name128(str);
    EVT_ASSERT(to_string() == string(str), name_type_exception,
               "Name128 not properly normalized (name: ${name}, normalized: ${normalized}) ",
               ("name", string(str))("normalized", to_string()));
}

name::operator string() const {
    static const char* charmap = ".12345abcdefghijklmnopqrstuvwxyz";

    string str(13, '.');

    uint64_t tmp = value;
    for(uint32_t i = 0; i <= 12; ++i) {
        char c      = charmap[tmp & (i == 0 ? 0x0f : 0x1f)];
        str[12 - i] = c;
        tmp >>= (i == 0 ? 4 : 5);
    }

    boost::algorithm::trim_right_if(str, [](char c) { return c == '.'; });
    return str;
}

name128::operator string() const {
    static const char* charmap = ".-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    string str(21, '.');

    uint128_t tmp = value;
    tmp >>= 2;
    for(uint32_t i = 0; i <= 20; ++i) {
        char c      = charmap[tmp & 0x3f];
        str[20 - i] = c;
        tmp >>= 6;
    }

    boost::algorithm::trim_right_if(str, [](char c) { return c == '.'; });
    return str;
}

}}  // namespace evt::chain

namespace fc {
void
to_variant(const evt::chain::name& c, fc::variant& v) {
    v = std::string(c);
}
void
from_variant(const fc::variant& v, evt::chain::name& check) {
    check = v.get_string();
}
void
to_variant(const evt::chain::name128& c, fc::variant& v) {
    v = std::string(c);
}
void
from_variant(const fc::variant& v, evt::chain::name128& check) {
    check = v.get_string();
}
}  // namespace fc
