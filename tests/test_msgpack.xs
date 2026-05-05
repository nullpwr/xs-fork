-- msgpack encode/decode round-trip tests

import msgpack

-- null round-trip
let encoded = msgpack.encode(null)
assert(len(encoded) > 0, "encoded null should have bytes")
let decoded = msgpack.decode(encoded)
assert(decoded == null, "null round-trip")

-- bool round-trip
let t_enc = msgpack.encode(true)
let t_dec = msgpack.decode(t_enc)
assert(t_dec == true, "true round-trip")

let f_enc = msgpack.encode(false)
let f_dec = msgpack.decode(f_enc)
assert(f_dec == false, "false round-trip")

-- integer round-trips
let int_tests = [0, 1, 42, 127, 128, 255, 256, 1000, -1, -32, -128, -1000]
for val in int_tests {
    let enc = msgpack.encode(val)
    let dec = msgpack.decode(enc)
    assert_eq(dec, val)
}

-- float round-trip
let pi_enc = msgpack.encode(3.14)
let pi_dec = msgpack.decode(pi_enc)
assert(pi_dec > 3.13, "float round-trip low")
assert(pi_dec < 3.15, "float round-trip high")

-- string round-trip
let s_enc = msgpack.encode("hello world")
let s_dec = msgpack.decode(s_enc)
assert_eq(s_dec, "hello world")

-- empty string
let e_enc = msgpack.encode("")
let e_dec = msgpack.decode(e_enc)
assert_eq(e_dec, "")

-- array round-trip
let arr = [1, 2, 3, 4, 5]
let arr_enc = msgpack.encode(arr)
let arr_dec = msgpack.decode(arr_enc)
assert_eq(len(arr_dec), 5)
assert_eq(arr_dec[0], 1)
assert_eq(arr_dec[4], 5)

-- nested array
let nested = [[1, 2], [3, 4]]
let nest_enc = msgpack.encode(nested)
let nest_dec = msgpack.decode(nest_enc)
assert_eq(len(nest_dec), 2)
assert_eq(len(nest_dec[0]), 2)
assert_eq(nest_dec[1][1], 4)

-- map round-trip
let m = {"name": "xs", "version": 1}
let m_enc = msgpack.encode(m)
let m_dec = msgpack.decode(m_enc)
assert_eq(m_dec["name"], "xs")
assert_eq(m_dec["version"], 1)

-- mixed types in array
let mixed = [1, "two", true, null, 3.5]
let mix_enc = msgpack.encode(mixed)
let mix_dec = msgpack.decode(mix_enc)
assert_eq(mix_dec[0], 1)
assert_eq(mix_dec[1], "two")
assert_eq(mix_dec[2], true)
assert(mix_dec[3] == null, "null in array")

-- stream encode/decode
let values = [1, "hello", true, [1, 2, 3]]
let stream_enc = msgpack.encode_stream(values)
let stream_dec = msgpack.decode_stream(stream_enc)
assert_eq(len(stream_dec), 4)
assert_eq(stream_dec[0], 1)
assert_eq(stream_dec[1], "hello")
assert_eq(stream_dec[2], true)

-- size helper
let size = msgpack.size(42)
assert(size > 0, "size should be positive")

let big_size = msgpack.size([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
assert(big_size > size, "array should be bigger than int")

-- roundtrip helper
let rt = msgpack.roundtrip({"key": [1, 2, 3]})
assert_eq(rt["key"][0], 1)
assert_eq(rt["key"][2], 3)

println("  msgpack: all tests passed")
