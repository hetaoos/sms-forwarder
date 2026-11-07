/*
 * 纯 JS SHA-256（登录挑战应答用，约 2KB）。
 * crypto.subtle 只在安全上下文（HTTPS / localhost）暴露；设备是 http://<局域网IP>:80，
 * 用 IP 访问时 window.crypto.subtle === undefined，因此自带实现作为默认路径。
 * 输出 64 位小写 hex；输入按 UTF-8 编码，不依赖 TextEncoder / secure context。
 * 协议见 ROADMAP.md §3.1：verifier = SHA256(salt + ":" + pass)，
 * proof = SHA256(verifier + ":" + ts + ":" + nonce)。
 */
(function (global) {
  'use strict';

  var K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ];
  var H0 = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  ];

  function utf8Bytes(str) {
    var out = [], i, c, cp;
    for (i = 0; i < str.length; i++) {
      c = str.charCodeAt(i);
      if (c < 0x80) {
        out.push(c);
      } else if (c < 0x800) {
        out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
      } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length &&
                 str.charCodeAt(i + 1) >= 0xdc00 && str.charCodeAt(i + 1) <= 0xdfff) {
        cp = 0x10000 + ((c - 0xd800) << 10) + (str.charCodeAt(++i) - 0xdc00);
        out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f),
                 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
      } else if (c >= 0x800) {
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
      } else {
        out.push(0xef, 0xbf, 0xbd); // 孤立低代理项 -> U+FFFD
      }
    }
    return out;
  }

  function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }
  function hex32(x) { return ('00000000' + (x >>> 0).toString(16)).slice(-8); }

  function sha256Hex(text) {
    var bytes = utf8Bytes(String(text));
    var bitLen = bytes.length * 8;
    var bitLenHi = Math.floor(bitLen / 4294967296);
    var bitLenLo = bitLen >>> 0;
    bytes.push(0x80);
    while (bytes.length % 64 !== 56) bytes.push(0);
    bytes.push((bitLenHi >>> 24) & 0xff, (bitLenHi >>> 16) & 0xff,
               (bitLenHi >>> 8) & 0xff, bitLenHi & 0xff);
    bytes.push((bitLenLo >>> 24) & 0xff, (bitLenLo >>> 16) & 0xff,
               (bitLenLo >>> 8) & 0xff, bitLenLo & 0xff);

    var h = H0.slice(), w = new Array(64);
    var i, j, a, b, c, d, e, f, g, hh, t1, t2, s0, s1, ch, maj;
    for (i = 0; i < bytes.length; i += 64) {
      for (j = 0; j < 16; j++) {
        w[j] = ((bytes[i + 4 * j] << 24) | (bytes[i + 4 * j + 1] << 16) |
                (bytes[i + 4 * j + 2] << 8) | bytes[i + 4 * j + 3]) >>> 0;
      }
      for (j = 16; j < 64; j++) {
        s0 = rotr(w[j - 15], 7) ^ rotr(w[j - 15], 18) ^ (w[j - 15] >>> 3);
        s1 = rotr(w[j - 2], 17) ^ rotr(w[j - 2], 19) ^ (w[j - 2] >>> 10);
        w[j] = (w[j - 16] + s0 + w[j - 7] + s1) >>> 0;
      }
      a = h[0]; b = h[1]; c = h[2]; d = h[3];
      e = h[4]; f = h[5]; g = h[6]; hh = h[7];
      for (j = 0; j < 64; j++) {
        s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        ch = (e & f) ^ (~e & g);
        t1 = (hh + s1 + ch + K[j] + w[j]) >>> 0;
        s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = (s0 + maj) >>> 0;
        hh = g; g = f; f = e; e = (d + t1) >>> 0;
        d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0;
      h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
      h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0;
      h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
    }
    return h.map(hex32).join('');
  }

  global.SmsfSha256 = { hex: sha256Hex, utf8: utf8Bytes };
})(typeof globalThis !== 'undefined' ? globalThis : this);
