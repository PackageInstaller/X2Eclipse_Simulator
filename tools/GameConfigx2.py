import os
import sys
import struct
import hashlib
import json
import re
from Crypto.Cipher import AES
from Crypto.Hash import MD5

DECODE_BUFFER_SIZE = 2048
AES_KEY_STRING = "x2_GAME_ds"
MAGIC_KEY = 4289702650
INT_MAX = 0x7FFFFFFF
MSEED = 161803398
CONFIG_NAME = "GameConfigx2"


class Random:
    def __init__(self, seed: int):
        if seed == -INT_MAX - 1:
            seed = INT_MAX
        seed = abs(seed)

        self._seed = [0] * 56
        mj = MSEED - seed
        if mj < 0:
            mj += INT_MAX
        self._seed[55] = mj
        mk = 1
        for i in range(1, 55):
            ii = (21 * i) % 55
            self._seed[ii] = mk
            mk = mj - mk
            if mk < 0:
                mk += INT_MAX
            mj = self._seed[ii]
        for _ in range(4):
            for i in range(1, 56):
                self._seed[i] -= self._seed[1 + (i + 30) % 55]
                if self._seed[i] < 0:
                    self._seed[i] += INT_MAX
        self._inext = 0
        self._inextp = 31

    def next_int(self) -> int:
        self._inext += 1
        if self._inext >= 56:
            self._inext = 1
        self._inextp += 1
        if self._inextp >= 56:
            self._inextp = 1
        mj = self._seed[self._inext] - self._seed[self._inextp]
        if mj < 0:
            mj += INT_MAX
        self._seed[self._inext] = mj
        return mj


def _align_json_bytes(raw: bytes) -> bytes:
    try:
        data = json.loads(raw.decode("utf-8"))
        if "a" not in data or not isinstance(data["a"], str):
            raise ValueError('缺少字段 "a"')
    except json.JSONDecodeError:
        raise ValueError("无效的 json 格式")

    original_text = raw.decode("utf-8")

    while True:
        current_bytes = original_text.encode("utf-8")
        r = len(current_bytes) % 16
        if r == 0:
            return current_bytes

        current_data = json.loads(original_text)

        if r < 8:
            if len(current_data["a"]) >= r:
                current_data["a"] = current_data["a"][:-r]
            else:
                current_data["a"] += "b" * (16 - r)
        else:
            current_data["a"] += "b" * (16 - r)

        pattern = r'("a"\s*:\s*")([^"]*?)(")'

        def replace_a_value(match):
            return match.group(1) + current_data["a"] + match.group(3)

        original_text = re.sub(pattern, replace_a_value, original_text)

        try:
            json.loads(original_text)
        except json.JSONDecodeError:
            return json.dumps(
                current_data, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")


# 由于是ecb所以用不到
def gen_iv(total_len: int) -> bytes:
    rnd = Random(43770 * total_len)
    return b"".join(struct.pack("<I", rnd.next_int()) for _ in range(4))


def gen_key(total_len: int) -> bytes:
    s = AES_KEY_STRING + str(MAGIC_KEY * total_len)
    return MD5.new(s.encode("utf-8")).digest()


def decrypt_data(enc: bytes) -> bytes:
    key = gen_key(len(enc))
    cipher = AES.new(key, AES.MODE_ECB)
    return b"".join(
        cipher.decrypt(enc[i : i + DECODE_BUFFER_SIZE])
        for i in range(0, len(enc), DECODE_BUFFER_SIZE)
    )


def encrypt_data(json_bytes: bytes) -> bytes:
    aligned = _align_json_bytes(json_bytes)
    key = gen_key(len(aligned))
    aes = AES.new(key, AES.MODE_ECB)
    return b"".join(
        aes.encrypt(aligned[i : i + DECODE_BUFFER_SIZE])
        for i in range(0, len(aligned), DECODE_BUFFER_SIZE)
    )


def main():
    if len(sys.argv) != 2:
        print("用法: python", os.path.basename(sys.argv[0]), "<输入文件>")
        sys.exit(1)

    in_path = sys.argv[1]
    if not os.path.exists(in_path):
        print("[!] 输入文件不存在:", in_path)
        sys.exit(2)

    ext = os.path.splitext(in_path)[1].lower()
    if ext == ".ab":
        out_path = f"{CONFIG_NAME}.json"
        enc = open(in_path, "rb").read()
        print(f"[*] 解密 {in_path} → {out_path}")
        plain = decrypt_data(enc)
        open(out_path, "wb").write(plain)
    else:
        with open(in_path, "rb") as f:
            plain = f.read()
        md5_name = hashlib.md5(CONFIG_NAME.encode("utf-8")).hexdigest() + ".ab"
        out_path = md5_name
        print(f"[*] 加密 {in_path} → {out_path}")
        enc = encrypt_data(plain)
        open(out_path, "wb").write(enc)


if __name__ == "__main__":
    main()
