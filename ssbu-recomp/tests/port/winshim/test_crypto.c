/* test_crypto.c - the hashes the runtime asks the platform for, against the published vectors:
 *   - BCryptHash(SHA-256) of "bcrypt.dll" (plat_posix.c's own SHA-256, no library): what nnro.c checks an NRR's module hashes
 *     with (nn::ro::LoadModule refuses a module whose hash is not registered, 0x1216) and nnmisc.c's nn::crypto SHA-256
 *     (FIPS 180-4 examples, every length 0..300 against a reference computed with Python's hashlib);
 *   - CryptCreateHash / CryptHashData / CryptGetHashParam with CALG_SHA1 and CALG_MD5 (nn::crypto::GenerateSha1Hash and the
 *     Md5 context of nnmisc.c, on OpenSSL's libcrypto in plat_posix.c): RFC 3174 and RFC 1321 vectors, GetHash that does not
 *     end the hash, CryptGenRandom.
 * The Crypt* half needs libcrypto at run time; the run without it (tests/port/native_env.sh finds none) prints that and
 * skips it. Was tests/port/modules/test_sha256.c (runtime/sha256.h, built in) and tests/port/misc/test_misc.c (nnmisc_posix.h's
 * own SHA-1 / MD5) of linux-arm64, which had no OpenSSL dependency; the Frame uses upstream's layer (OpenSSL, SteamOS has it). */
#include "tp.h"
#include <string.h>
#include "plat/wincrypt.h"

static void hex(const uint8_t *d, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static int is(const uint8_t *d, size_t n, const char *want) { char s[65]; hex(d, n, s); if (strcmp(s, want)) fprintf(stderr, "    got  %s\n    want %s\n", s, want); return !strcmp(s, want); }

typedef LONG (*OpenFn)(void **, LPCWSTR, LPCWSTR, ULONG);
typedef LONG (*HashFn)(void *, uint8_t *, ULONG, const uint8_t *, ULONG, uint8_t *, ULONG);
typedef LONG (*CloseFn)(void *, ULONG);
static HashFn g_hash;
static void *g_alg;
static void sha256(const void *m, size_t n, uint8_t out[32]) { memset(out, 0, 32); CHECK(g_hash(g_alg, NULL, 0, m, (ULONG)n, out, 32) == 0); }

static void sha_check(const char *msg, size_t rep, int alg, const char *want) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    uint8_t d[64];
    DWORD len = alg == CALG_MD5 ? 16 : alg == CALG_SHA1 ? 20 : 32;
    CHECK(CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT));
    CHECK(CryptCreateHash(prov, (ALG_ID)alg, 0, 0, &h));
    int fed = 1;
    for (size_t i = 0; i < rep; i++) fed &= CryptHashData(h, (const BYTE *)msg, (DWORD)strlen(msg), 0);
    CHECK(fed);
    DWORD n = len;
    CHECK(CryptGetHashParam(h, HP_HASHVAL, d, &n, 0) && n == len);
    CHECKF(is(d, len, want), "hash of \"%.20s\" x%zu", msg, rep);
    CryptDestroyHash(h);
    CryptReleaseContext(prov, 0);
}

int main(void) {
    SECTION("BCryptHash SHA-256 (built in): FIPS 180 examples");
    HMODULE bc = LoadLibraryA("bcrypt.dll");
    OpenFn open_ = bc ? (OpenFn)(void *)GetProcAddress(bc, "BCryptOpenAlgorithmProvider") : NULL;
    CloseFn close_ = bc ? (CloseFn)(void *)GetProcAddress(bc, "BCryptCloseAlgorithmProvider") : NULL;
    g_hash = bc ? (HashFn)(void *)GetProcAddress(bc, "BCryptHash") : NULL;
    CHECK(open_ && close_ && g_hash);
    if (!open_ || !close_ || !g_hash) return tp_done("test_crypto");
    CHECK(open_(&g_alg, L"SHA256", NULL, 0) == 0);
    uint8_t h[32];
    sha256("", 0, h);
    CHECK(is(h, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    sha256("abc", 3, h);
    CHECK(is(h, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m448, strlen(m448), h);
    CHECK(is(h, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha256(m896, strlen(m896), h);
    CHECK(is(h, 32, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
    char *a = malloc(1000000);
    memset(a, 'a', 1000000);
    sha256(a, 1000000, h);
    CHECK(is(h, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    {   uint8_t buf[200];                                              /* unaligned input */
        memcpy(buf + 3, "abc", 3);
        sha256(buf + 3, 3, h);
        CHECK(is(h, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")); }
    free(a);
    uint8_t small[8];
    CHECK(g_hash(g_alg, NULL, 0, "abc", 3, small, 8) != 0);            /* an output buffer that is too small is refused */
    SECTION("every length 0..300 (padding edges 55 / 56 / 63 / 64 / 119 / 120 ...): the digest of the 301 digests");
    {   uint8_t msg[300], d[32], all[301 * 32];
        for (int i = 0; i < 300; i++) msg[i] = (uint8_t)((i * 7 + 3) & 0xff);
        for (size_t n = 0; n <= 300; n++) { sha256(msg, n, d); memcpy(all + n * 32, d, 32); }
        sha256(all, sizeof all, h);
        CHECK(is(h, 32, "7d917fbd2cf49ddff9ad0a8706bba32d204e92e71d2e369c5a03d6af29278c9f")); }
    close_(g_alg, 0);

    SECTION("CryptoAPI hashes on libcrypto: SHA-1 (RFC 3174 / FIPS 180), MD5 (RFC 1321), SHA-256");
    HCRYPTPROV prov = 0;
    HCRYPTHASH probe = 0;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) || !CryptCreateHash(prov, CALG_SHA1, 0, 0, &probe)) {
        printf("    no libcrypto here (the EVP stand-ins refuse): skipped\n");
        return tp_done("test_crypto");
    }
    CryptDestroyHash(probe);
    sha_check("abc", 1, CALG_SHA1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha_check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, CALG_SHA1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    sha_check("a", 1000000, CALG_SHA1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    sha_check("0123456701234567012345670123456701234567012345670123456701234567", 10, CALG_SHA1, "dea356a2cddd90c7a7ecedc5ebb563934f460452");
    sha_check("", 1, CALG_SHA1, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha_check("", 1, CALG_MD5, "d41d8cd98f00b204e9800998ecf8427e");
    sha_check("a", 1, CALG_MD5, "0cc175b9c0f1b6a831c399e269772661");
    sha_check("abc", 1, CALG_MD5, "900150983cd24fb0d6963f7d28e17f72");
    sha_check("message digest", 1, CALG_MD5, "f96b697d7cb7938d525a2f31aaf161d0");
    sha_check("abcdefghijklmnopqrstuvwxyz", 1, CALG_MD5, "c3fcd3d76192e4007dfb496cca67e13b");
    sha_check("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 1, CALG_MD5, "d174ab98d277d9f5a5611c2c9f419d9f");
    sha_check("1234567890", 8, CALG_MD5, "57edf4a22be3c955ac49da2e2107b67a");
    sha_check("abc", 1, CALG_SHA_256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    SECTION("MD5: GetHash does not end the hash (update after getting the value continues)");
    {   HCRYPTHASH hh = 0;
        uint8_t d[16], e[16];
        DWORD n = 16;
        CHECK(CryptCreateHash(prov, CALG_MD5, 0, 0, &hh));
        CHECK(CryptHashData(hh, (const BYTE *)"a", 1, 0) && CryptGetHashParam(hh, HP_HASHVAL, d, &n, 0));
        n = 16;
        CHECK(CryptHashData(hh, (const BYTE *)"bc", 2, 0) && CryptGetHashParam(hh, HP_HASHVAL, e, &n, 0));
        CHECK(is(d, 16, "0cc175b9c0f1b6a831c399e269772661") && is(e, 16, "900150983cd24fb0d6963f7d28e17f72"));
        CryptDestroyHash(hh); }
    SECTION("CryptGenRandom");
    {   static uint8_t x[4096], y[4096];
        int zeros = 0, same = 0;
        CHECK(CryptGenRandom(prov, sizeof x, x) && CryptGenRandom(prov, sizeof y, y));
        for (int i = 0; i < 4096; i++) { zeros += x[i] == 0; same += x[i] == y[i]; }
        CHECKF(zeros < 60, "%d zero bytes in 4096 (expect ~16)", zeros);
        CHECKF(same < 60, "%d equal bytes between two draws (expect ~16)", same);
        CHECK(CryptGenRandom(prov, 0, x)); }
    CryptReleaseContext(prov, 0);
    return tp_done("test_crypto");
}
