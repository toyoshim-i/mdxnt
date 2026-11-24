/// Default fallback 26-byte OPN FM voice (matches Python mdxnt_compile.py).
pub const DEFAULT_VOICE: [u8; 26] = [
    0x01, 0x01, 0x01, 0x01,
    0x18, 0x18, 0x18, 0x00,
    0x1F, 0x1F, 0x1F, 0x1F,
    0x08, 0x08, 0x08, 0x06,
    0x02, 0x02, 0x02, 0x02,
    0x27, 0x27, 0x27, 0x27,
    0x34, 0x00,
];

#[derive(Debug, Clone)]
pub struct VoiceInfo {
    pub id: usize,
    pub format: &'static str,
    pub fb: u8,
    pub con: u8,
    pub bytes: [u8; 26],
    pub def_line: u32,
}

/// Pack an OPN 4-operator FM voice into 26 bytes:
///   [0..3]   DT1/MUL (4 ops in slot order 1,3,2,4 -> reg 0x30,0x34,0x38,0x3C)
///   [4..7]   TL      (reg 0x40..0x4C)
///   [8..11]  KS/AR   (reg 0x50..0x5C)
///   [12..15] DR      (reg 0x60..0x6C)
///   [16..19] SR      (reg 0x70..0x7C)
///   [20..23] SL/RR   (reg 0x80..0x8C)
///   [24]     FB/CON  (reg 0xB0)
///   [25]     pad (0)
///
/// Accepts 38 ints (MUCOM/OPN order: FB, CON, then 4 ops x [AR,DR,SR,RR,SL,TL,KS,ML,DT])
/// OR 47 ints (MDX @N order: CON, FL, slot_mask, then 4 ops x [AR,D1R,D2R,RR,D1L,TL,KS,MUL,DT1,DT2,AME]).
pub fn pack_opn_voice(nums: &[i32]) -> Result<([u8; 26], u8, u8, &'static str), String> {
    let mut out = [0u8; 26];
    if nums.len() == 38 {
        let fb = (nums[0] & 7) as u8;
        let con = (nums[1] & 7) as u8;
        let order = [0usize, 2, 1, 3];
        for (slot, &idx) in order.iter().enumerate() {
            let base = 2 + idx * 9;
            let ar = nums[base] & 31;
            let d1r = nums[base + 1] & 31;
            let d2r = nums[base + 2] & 31;
            let rr = nums[base + 3] & 15;
            let sl = nums[base + 4] & 15;
            let tlv = nums[base + 5] & 0x7F;
            let ks = nums[base + 6] & 3;
            let ml = nums[base + 7] & 15;
            let dt = nums[base + 8] & 7;
            out[slot] = ((dt << 4) | ml) as u8;
            out[4 + slot] = tlv as u8;
            out[8 + slot] = ((ks << 6) | ar) as u8;
            out[12 + slot] = d1r as u8;
            out[16 + slot] = d2r as u8;
            out[20 + slot] = ((sl << 4) | rr) as u8;
        }
        out[24] = (fb << 3) | con;
        out[25] = 0;
        Ok((out, fb, con, "OPN/MUCOM (38)"))
    } else if nums.len() == 47 {
        let con = (nums[0] & 7) as u8;
        let fb = (nums[1] & 7) as u8;
        for slot in 0..4 {
            let base = 3 + slot * 11;
            let ar = nums[base] & 31;
            let d1r = nums[base + 1] & 31;
            let d2r = nums[base + 2] & 31;
            let rr = nums[base + 3] & 15;
            let sl = nums[base + 4] & 15;
            let tlv = nums[base + 5] & 0x7F;
            let ks = nums[base + 6] & 3;
            let ml = nums[base + 7] & 15;
            let dt1 = nums[base + 8] & 7;
            out[slot] = ((dt1 << 4) | ml) as u8;
            out[4 + slot] = tlv as u8;
            out[8 + slot] = ((ks << 6) | ar) as u8;
            out[12 + slot] = d1r as u8;
            out[16 + slot] = d2r as u8;
            out[20 + slot] = ((sl << 4) | rr) as u8;
        }
        out[24] = (fb << 3) | con;
        out[25] = 0;
        Ok((out, fb, con, "MDX (47)"))
    } else {
        Err(format!(
            "FM voice expects 38 (OPN/MUCOM) or 47 (MDX) integers, got {}",
            nums.len()
        ))
    }
}
