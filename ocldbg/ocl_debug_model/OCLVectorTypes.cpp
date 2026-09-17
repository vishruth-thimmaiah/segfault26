#include "OCLVectorTypes.h"

#include <array>
#include <cstring>

namespace ocldbg {

size_t ocl_vector_storage_count(const OCLVectorType &vt) {
    return vt.count == 3 ? 4 : vt.count;
}

std::optional<OCLVectorType> parse_ocl_vector_type(std::string_view type_name) {
    struct Elem {
        std::string_view name;
        size_t size;
        bool is_float;
        bool is_signed;
    };
    constexpr std::array<Elem, 11> kElems{{
        {.name = "double", .size = 8, .is_float = true, .is_signed = false},
        {.name = "float", .size = 4, .is_float = true, .is_signed = false},
        {.name = "half", .size = 2, .is_float = true, .is_signed = false},
        {.name = "ulong", .size = 8, .is_float = false, .is_signed = false},
        {.name = "long", .size = 8, .is_float = false, .is_signed = true},
        {.name = "uint", .size = 4, .is_float = false, .is_signed = false},
        {.name = "int", .size = 4, .is_float = false, .is_signed = true},
        {.name = "ushort", .size = 2, .is_float = false, .is_signed = false},
        {.name = "short", .size = 2, .is_float = false, .is_signed = true},
        {.name = "uchar", .size = 1, .is_float = false, .is_signed = false},
        {.name = "char", .size = 1, .is_float = false, .is_signed = true},
    }};

    for (const auto &elem : kElems) {
        if (!type_name.starts_with(elem.name)) {
            continue;
        }
        std::string_view suffix = type_name.substr(elem.name.size());
        size_t count = 0;
        if (suffix == "2") {
            count = 2;
        } else if (suffix == "3") {
            count = 3;
        } else if (suffix == "4") {
            count = 4;
        } else if (suffix == "8") {
            count = 8;
        } else if (suffix == "16") {
            count = 16;
        } else {
            continue;
        }
        return OCLVectorType{.elem_size = elem.size,
                             .count = count,
                             .is_float = elem.is_float,
                             .is_signed = elem.is_signed};
    }
    return std::nullopt;
}

namespace {

std::string decode_vector_lane(const uint8_t *bytes, const OCLVectorType &vt) {
    if (vt.is_float) {
        if (vt.elem_size == 4) {
            float v = 0.0F;
            std::memcpy(&v, bytes, sizeof(v));
            return std::to_string(v);
        }
        if (vt.elem_size == 8) {
            double v = 0.0;
            std::memcpy(&v, bytes, sizeof(v));
            return std::to_string(v);
        }
        return "?"; // half: no native decode target
    }
    if (vt.is_signed) {
        switch (vt.elem_size) {
        case 1: {
            int8_t v = 0;
            std::memcpy(&v, bytes, 1);
            return std::to_string(v);
        }
        case 2: {
            int16_t v = 0;
            std::memcpy(&v, bytes, 2);
            return std::to_string(v);
        }
        case 4: {
            int32_t v = 0;
            std::memcpy(&v, bytes, 4);
            return std::to_string(v);
        }
        default: {
            int64_t v = 0;
            std::memcpy(&v, bytes, 8);
            return std::to_string(v);
        }
        }
    }
    switch (vt.elem_size) {
    case 1: {
        uint8_t v = 0;
        std::memcpy(&v, bytes, 1);
        return std::to_string(v);
    }
    case 2: {
        uint16_t v = 0;
        std::memcpy(&v, bytes, 2);
        return std::to_string(v);
    }
    case 4: {
        uint32_t v = 0;
        std::memcpy(&v, bytes, 4);
        return std::to_string(v);
    }
    default: {
        uint64_t v = 0;
        std::memcpy(&v, bytes, 8);
        return std::to_string(v);
    }
    }
}

} // namespace

std::string format_ocl_vector_bytes(const std::vector<uint8_t> &bytes, const OCLVectorType &vt) {
    std::string out = "(";
    for (size_t i = 0; i < vt.count; ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += decode_vector_lane(bytes.data() + (i * vt.elem_size), vt);
    }
    out += ")";
    return out;
}

} // namespace ocldbg
