use std::{
    sync::atomic::{AtomicI64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use chrono::DateTime;

const WIN_EPOCH: i64 = 11_644_473_600;
const TICKS_PER_SECOND: u64 = 10_000_000;

// FIPS 180-4 / Edge Sec-MS-GEC v2 modified SHA-256 constants
const SHA256_INITIAL: [u32; 8] = [
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
];

const SHA256_STATE: [u32; 8] = [
    0xFD9FBEA2, 0x91243CE7, 0xBE499094, 0xCD2E1E0A,
    0x6C006F49, 0x04BD35AB, 0xFEB0A7E4, 0x03EFFBD2,
];

const SHA256_SCHEDULE: [u32; 14] = [
    0xEB24BF9E, 0x1558691F, 0xA0C09EE2, 0x0CFA32BA,
    0xA622EE8C, 0xD1DD9491, 0xC0DD25A0, 0xF9E70AB6,
    0xF0C31573, 0x7058CE4E, 0x7188E606, 0x00E5AB33,
    0xD258B060, 0x72766564,
];

const SHA256_ROUND_CONSTANTS: [u32; 64] = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
];

const SHA256_ADJUSTMENT: u32 = 0xC9DC06A7;
const SHA256_SIGMA0_ADJUSTMENT: u32 = 0x059C0B80;
const SHA256_SIGMA1_ADJUSTMENT: u32 = 0x000B92C0;

const CS64_COEFFICIENTS: [(u32, u32); 14] = [
    (0xE82E91B1, 0xB32700D0),
    (0x44DE24C6, 0x88DFF51D),
    (0x3BEB65BC, 0x29514038),
    (0xC89C5933, 0xB328CBDF),
    (0x8D1EC211, 0xD405B4D5),
    (0x314EF5D5, 0x26E55F3C),
    (0xE92491B1, 0x607801C6),
    (0xF0A5B9D5, 0x82B2C1FB),
    (0x06064CF3, 0xFA51A3DA),
    (0xDA79FA60, 0x207CC464),
    (0xBED20560, 0x3C3C74F3),
    (0x89F9C9F8, 0xD2D14B61),
    (0x35FA38C6, 0xBB04A2C1),
    (0xCB49E3C6, 0xC5626167),
];

const CS64_FACTOR: u32 = 0x82893933;
const CS64_ADJUSTMENT0: u32 = 0xF2F90B1A;
const CS64_ADJUSTMENT1: u32 = 0x0B1A0000;

static CLOCK_SKEW_SECS: AtomicI64 = AtomicI64::new(0);

fn sigma0(x: u32) -> u32 {
    x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3)
}

fn sigma1(x: u32) -> u32 {
    x.rotate_right(17) ^ x.rotate_right(19) ^ (x >> 10)
}

fn expand_sha256_schedule(words: &mut [u32; 64], first_word: usize) {
    for index in first_word..64 {
        words[index] = sigma1(words[index - 2])
            .wrapping_add(words[index - 7])
            .wrapping_add(sigma0(words[index - 15]))
            .wrapping_add(words[index - 16]);
    }
}

fn sha256_rounds(state: [u32; 8], words: &[u32; 64], first_round: usize) -> [u32; 8] {
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = state;
    for index in first_round..64 {
        let constant = if first_round == 14 && index == 14 {
            SHA256_ADJUSTMENT
        } else {
            SHA256_ROUND_CONSTANTS[index]
        };
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = g ^ (e & (f ^ g));
        let t1 = h
            .wrapping_add(s1)
            .wrapping_add(ch)
            .wrapping_add(constant)
            .wrapping_add(words[index]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & b) | (c & (a | b));
        let t2 = s0.wrapping_add(maj);

        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(t1);
        d = c;
        c = b;
        b = a;
        a = t1.wrapping_add(t2);
    }
    [a, b, c, d, e, f, g, h]
}

fn sha256_init(prefix: &[u8]) -> [u32; 8] {
    let mut words = [0u32; 64];
    words[14] = u32::from_be_bytes([0, prefix[0], prefix[1], prefix[2]]);
    words[15] = u32::from_be_bytes([prefix[3], prefix[4], prefix[5], prefix[6]]);

    for index in 16..30 {
        let mut value = SHA256_SCHEDULE[index - 16];
        if index - 2 >= 14 {
            let s1 = sigma1(words[index - 2]);
            value = value.wrapping_add(if index == 16 {
                s1 ^ SHA256_SIGMA1_ADJUSTMENT
            } else {
                s1
            });
        }
        if index - 7 >= 14 {
            value = value.wrapping_add(words[index - 7]);
        }
        if index - 15 >= 14 {
            let s0 = sigma0(words[index - 15]);
            value = value.wrapping_add(if index == 29 {
                s0 ^ SHA256_SIGMA0_ADJUSTMENT
            } else {
                s0
            });
        }
        words[index] = value;
    }

    words[30] = sigma1(words[28])
        .wrapping_add(words[23])
        .wrapping_add(sigma0(words[15]))
        .wrapping_add(words[14])
        .wrapping_add(SHA256_ADJUSTMENT)
        .wrapping_sub(SHA256_ROUND_CONSTANTS[14]);

    expand_sha256_schedule(&mut words, 31);
    let working = sha256_rounds(SHA256_STATE, &words, 14);
    std::array::from_fn(|i| SHA256_INITIAL[i].wrapping_add(working[i]))
}

fn sha256_digest(data: &[u8]) -> [u8; 32] {
    debug_assert!(data.len() >= 7);
    let length = 57 + data.len();
    let pad_zeros = (55isize - length as isize).rem_euclid(64) as usize;

    let mut padded = Vec::with_capacity((data.len() - 7) + 1 + pad_zeros + 8);
    padded.extend_from_slice(&data[7..]);
    padded.push(0x80);
    padded.resize(padded.len() + pad_zeros, 0);
    padded.extend_from_slice(&((length as u64) * 8).to_be_bytes());

    let mut state = sha256_init(&data[..7]);
    for block in padded.chunks_exact(64) {
        let mut words = [0u32; 64];
        for (i, chunk) in block.chunks_exact(4).enumerate() {
            words[i] = u32::from_be_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        }
        expand_sha256_schedule(&mut words, 16);
        let working = sha256_rounds(state, &words, 0);
        for i in 0..8 {
            state[i] = state[i].wrapping_add(working[i]);
        }
    }

    let mut out = [0u8; 32];
    for (i, word) in state.iter().enumerate() {
        out[i * 4..(i + 1) * 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

fn words_le4_even(data: &[u8]) -> Vec<u32> {
    let mut padded = Vec::with_capacity(data.len() + 4);
    padded.push(0);
    padded.extend_from_slice(data);
    while padded.len() % 4 != 0 {
        padded.push(0);
    }
    let mut words: Vec<u32> = padded
        .chunks_exact(4)
        .map(|chunk| u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect();
    if words.len() % 2 != 0 {
        words.push(0);
    }
    words
}

fn cs64_digest(data: &[u8], digest: &[u8; 32]) -> (u32, u32) {
    let key_a = u32::from_le_bytes([digest[0], digest[1], digest[2], digest[3]]) | 1;
    let key_b = u32::from_le_bytes([digest[4], digest[5], digest[6], digest[7]]) | 1;
    let factors = [
        key_a.wrapping_mul(CS64_FACTOR),
        key_b.wrapping_mul(CS64_FACTOR),
    ];

    let input_words = words_le4_even(data);
    let mut terms = Vec::with_capacity(CS64_COEFFICIENTS.len() + input_words.len());
    let mut offsets = Vec::with_capacity(CS64_COEFFICIENTS.len() + input_words.len());

    for (index, &(coefficient, offset)) in CS64_COEFFICIENTS.iter().enumerate() {
        terms.push(coefficient.wrapping_mul(factors[index % 2]));
        offsets.push(offset);
    }

    for (index, word) in input_words.into_iter().enumerate() {
        let mut term = word.wrapping_mul(if index % 2 == 0 { key_a } else { key_b });
        let mut offset = word
            .rotate_left(16)
            .wrapping_mul(if index % 2 == 0 { 0xEF0569FB } else { 0xC31713DB });
        if index == 0 {
            term = term.wrapping_add(CS64_ADJUSTMENT0.wrapping_mul(factors[0]));
            offset = offset.wrapping_add(CS64_ADJUSTMENT1);
        }
        terms.push(term);
        offsets.push(offset);
    }

    let mut swap_acc: u32 = 0;
    let mut swap_last: u32 = 0;
    let mut state: u32 = 0;
    let mut acc: u32 = 0;

    for index in (0..terms.len()).step_by(2) {
        let x = terms[index];
        let y = terms[index + 1];

        let t = offsets[index].wrapping_add(x).rotate_left(16);
        let q = t.wrapping_mul(0x76F50F34);
        swap_acc = swap_acc.wrapping_add(q.rotate_left(16).wrapping_mul(0xC3EFEA97).wrapping_add(q));

        let t = offsets[index + 1].wrapping_add(y).rotate_left(16);
        let q = t.wrapping_mul(0x1881CE3C);
        swap_last = q.rotate_left(16).wrapping_mul(0x35BD1EC9).wrapping_add(q);
        swap_acc = swap_acc.wrapping_add(swap_last);

        state = state.wrapping_mul(key_a).wrapping_add(x);
        state = state.rotate_left(16).wrapping_mul(0xCF98B111);
        state = state.rotate_left(16).wrapping_mul(0x87085B9F);
        state = state.rotate_left(16).wrapping_mul(0x12CEB96D);
        state = state.rotate_left(16).wrapping_mul(0x257E1D83);
        acc = acc.wrapping_add(state);

        let t = state.wrapping_mul(key_b).wrapping_add(y);
        let t = t.rotate_left(16).wrapping_mul(0xA27416F5);
        let t = t.rotate_left(16).wrapping_mul(0xD38396FF);
        state = t
            .rotate_left(16)
            .wrapping_mul(0x7C932B89)
            .rotate_left(16)
            .wrapping_mul(0xBFA49F69);
        acc = acc.wrapping_add(state);
    }

    (swap_last ^ state, swap_acc ^ acc)
}

fn encode_hex_upper(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        use std::fmt::Write;
        let _ = write!(&mut out, "{byte:02X}");
    }
    out
}

fn current_unix_secs() -> Result<i64, String> {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|err| err.to_string())?
        .as_secs() as i64;
    Ok(secs + CLOCK_SKEW_SECS.load(Ordering::Relaxed))
}

pub fn parse_rfc2616_date(date: &str) -> Option<i64> {
    DateTime::parse_from_rfc2822(date.trim())
        .ok()
        .map(|dt| dt.timestamp())
}

pub fn adjust_clock_skew_from_date(server_date: &str) -> Result<(), String> {
    let server_secs = parse_rfc2616_date(server_date)
        .ok_or_else(|| format!("无法解析服务端时间: {server_date}"))?;
    let client_secs = current_unix_secs()?;
    CLOCK_SKEW_SECS.fetch_add(server_secs - client_secs, Ordering::Relaxed);
    Ok(())
}

pub fn generate_sec_ms_gec_v2_at(unix_secs: i64, trusted_client_token: &str) -> String {
    let win_secs = (unix_secs + WIN_EPOCH).max(0) as u64;
    let rounded_secs = win_secs - (win_secs % 300);
    let ticks = rounded_secs * TICKS_PER_SECOND;
    let str_to_hash = format!("{ticks}{trusted_client_token}");
    let bytes = str_to_hash.as_bytes();

    let digest = sha256_digest(bytes);
    let (w0, w1) = cs64_digest(bytes, &digest);
    let mut cs64_bytes = [0u8; 8];
    cs64_bytes[..4].copy_from_slice(&w0.to_le_bytes());
    cs64_bytes[4..].copy_from_slice(&w1.to_le_bytes());

    format!("{}-{}", encode_hex_upper(&cs64_bytes), encode_hex_upper(&digest))
}

pub fn generate_sec_ms_gec_v2(trusted_client_token: &str) -> Result<String, String> {
    Ok(generate_sec_ms_gec_v2_at(
        current_unix_secs()?,
        trusted_client_token,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEST_TOKEN: &str = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";

    #[test]
    fn v2_vectors_match_reference_edge_tts() {
        let cases = [
            (
                0,
                "52BC7A17B50DFEC9-28C4394775C8751CD2F6859C030C06DB1489ACAB3B3A7651831FC26895177ED6",
            ),
            (
                299,
                "52BC7A17B50DFEC9-28C4394775C8751CD2F6859C030C06DB1489ACAB3B3A7651831FC26895177ED6",
            ),
            (
                300,
                "F7535A20A262E331-28675BF85C7106BBF20035A78FCB0B6890813EAD5C3B983EF3424931B720BABB",
            ),
            (
                1_700_000_000,
                "2CD23A2CA0B2595A-F3C846C370E05F9CC83ACC5F995C5F5D87A4D0F14CE8936A0F902E64099E38A2",
            ),
            (
                1_735_689_600,
                "54404AFE67C9B44A-E72E51D8B4F5F75E9A96B7E20D7877227455BC310433BD30C817CD54A1D9313E",
            ),
            (
                1_764_300_000,
                "8CB225BB916E3AD7-97BC763EB1E1D334343D0E45B4177C1E42F93E2960936CA316CF6354335A9AB5",
            ),
            (
                1_795_760_857,
                "8B426540AC3642E7-2E40A33EDF07E2AFD846CC02E256F255CEB746B05FCFD864B2AC91B2B89350E5",
            ),
        ];

        for (ts, expected) in cases {
            assert_eq!(generate_sec_ms_gec_v2_at(ts, TEST_TOKEN), expected);
        }
    }

    #[test]
    fn parses_rfc2616_server_date_header() {
        assert_eq!(
            parse_rfc2616_date("Thu, 01 Jan 1970 00:00:00 GMT"),
            Some(0)
        );
        assert_eq!(
            parse_rfc2616_date("Sat, 28 Nov 2026 06:27:37 GMT"),
            Some(1_795_847_257)
        );
    }
}
