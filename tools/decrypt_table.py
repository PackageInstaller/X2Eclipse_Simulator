import sys, os, base64
from Crypto.PublicKey import RSA
from Crypto.Util.number import bytes_to_long, long_to_bytes

PUBLIC_KEY_B64 = "MIGdMA0GCSqGSIb3DQEBAQUAA4GLADCBhwKBgQCyJiTzABL2wironv9+4wnZTg7JXr1ekiMA3RdL2e+W8kEtyZgghb5KBBAASKuiGNxhadrnSgC8+h1r7B/JLudatvdlzwyy1gAs/mbVYHd7x1WoBfzDpWkZX8bhDO/uX4GnBhWAmtapbbjVGOAVIuaIV8lBzNXJ30mJPDI4wKc7/QIBAw=="

_rsa_key = None

def rsa_public_decrypt(block, pubkey_b64):
    global _rsa_key
    if _rsa_key is None:
        _rsa_key = RSA.import_key(base64.b64decode(pubkey_b64))
    key = _rsa_key
    n, e = key.n, key.e
    m = pow(bytes_to_long(block), e, n)
    block_len = (key.size_in_bits() + 7) // 8
    decrypted = long_to_bytes(m, block_len)
    if len(decrypted) >= 11 and decrypted[0] == 0:
        if decrypted[1] in (1, 2):
            idx = 2
            while idx < len(decrypted) and decrypted[idx] != 0:
                idx += 1
            if idx < len(decrypted):
                return decrypted[idx+1:]
    return decrypted

def convert_data_bytes(data):
    orig_len = len(data)
    if orig_len < 128:
        rsa_result = b''
    else:
        rsa_result = rsa_public_decrypt(data[:128], PUBLIC_KEY_B64)
    result = bytearray(rsa_result + data[128:])
    length = len(result)

    a = 0
    i = 1
    while i < length:
        result[i] ^= (a + i) & 0xFF
        i *= 2
        a -= 1
    b = 0
    j = length - 1
    while j > 0:
        result[j] ^= (b + j) & 0xFF
        j >>= 1
        b -= 1

    return bytes(result)

def main():

    with open(sys.argv[1], 'rb') as f:
        data = f.read()

    result = convert_data_bytes(data)

    with open(sys.argv[2], 'wb') as f:
        f.write(result)

    try:
        b = result[0]
        tag = b >> 3
        wt = b & 7
    except Exception as e:
        print(f"Error: {e}")

if __name__ == '__main__':
    main()
