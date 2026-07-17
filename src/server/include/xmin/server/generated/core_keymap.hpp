#ifndef XMIN_SERVER_GENERATED_CORE_KEYMAP_HPP
#define XMIN_SERVER_GENERATED_CORE_KEYMAP_HPP

// Fixed US keymap and input defaults retained from the original Xmin profile.
// This checked-in table is the canonical server input; no runtime XKB database
// or legacy-server generator is required.

#include <array>
#include <cstddef>
#include <cstdint>

namespace xmin::server {

inline constexpr std::uint8_t minimum_keycode = 8;
inline constexpr std::uint8_t maximum_keycode = 255;
inline constexpr std::size_t keysyms_per_keycode = 7;

inline constexpr auto core_keymap = [] {
    std::array<std::array<std::uint32_t, keysyms_per_keycode>, 256>
        result{};
    result[9] = {{
        0x0000ff1bU, 0x00000000U, 0x0000ff1bU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[10] = {{
        0x00000031U, 0x00000021U, 0x00000031U, 0x00000021U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[11] = {{
        0x00000032U, 0x00000040U, 0x00000032U, 0x00000040U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[12] = {{
        0x00000033U, 0x00000023U, 0x00000033U, 0x00000023U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[13] = {{
        0x00000034U, 0x00000024U, 0x00000034U, 0x00000024U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[14] = {{
        0x00000035U, 0x00000025U, 0x00000035U, 0x00000025U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[15] = {{
        0x00000036U, 0x0000005eU, 0x00000036U, 0x0000005eU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[16] = {{
        0x00000037U, 0x00000026U, 0x00000037U, 0x00000026U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[17] = {{
        0x00000038U, 0x0000002aU, 0x00000038U, 0x0000002aU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[18] = {{
        0x00000039U, 0x00000028U, 0x00000039U, 0x00000028U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[19] = {{
        0x00000030U, 0x00000029U, 0x00000030U, 0x00000029U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[20] = {{
        0x0000002dU, 0x0000005fU, 0x0000002dU, 0x0000005fU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[21] = {{
        0x0000003dU, 0x0000002bU, 0x0000003dU, 0x0000002bU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[22] = {{
        0x0000ff08U, 0x0000ff08U, 0x0000ff08U, 0x0000ff08U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[23] = {{
        0x0000ff09U, 0x0000fe20U, 0x0000ff09U, 0x0000fe20U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[24] = {{
        0x00000071U, 0x00000051U, 0x00000071U, 0x00000051U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[25] = {{
        0x00000077U, 0x00000057U, 0x00000077U, 0x00000057U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[26] = {{
        0x00000065U, 0x00000045U, 0x00000065U, 0x00000045U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[27] = {{
        0x00000072U, 0x00000052U, 0x00000072U, 0x00000052U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[28] = {{
        0x00000074U, 0x00000054U, 0x00000074U, 0x00000054U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[29] = {{
        0x00000079U, 0x00000059U, 0x00000079U, 0x00000059U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[30] = {{
        0x00000075U, 0x00000055U, 0x00000075U, 0x00000055U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[31] = {{
        0x00000069U, 0x00000049U, 0x00000069U, 0x00000049U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[32] = {{
        0x0000006fU, 0x0000004fU, 0x0000006fU, 0x0000004fU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[33] = {{
        0x00000070U, 0x00000050U, 0x00000070U, 0x00000050U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[34] = {{
        0x0000005bU, 0x0000007bU, 0x0000005bU, 0x0000007bU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[35] = {{
        0x0000005dU, 0x0000007dU, 0x0000005dU, 0x0000007dU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[36] = {{
        0x0000ff0dU, 0x00000000U, 0x0000ff0dU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[37] = {{
        0x0000ffe3U, 0x00000000U, 0x0000ffe3U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[38] = {{
        0x00000061U, 0x00000041U, 0x00000061U, 0x00000041U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[39] = {{
        0x00000073U, 0x00000053U, 0x00000073U, 0x00000053U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[40] = {{
        0x00000064U, 0x00000044U, 0x00000064U, 0x00000044U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[41] = {{
        0x00000066U, 0x00000046U, 0x00000066U, 0x00000046U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[42] = {{
        0x00000067U, 0x00000047U, 0x00000067U, 0x00000047U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[43] = {{
        0x00000068U, 0x00000048U, 0x00000068U, 0x00000048U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[44] = {{
        0x0000006aU, 0x0000004aU, 0x0000006aU, 0x0000004aU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[45] = {{
        0x0000006bU, 0x0000004bU, 0x0000006bU, 0x0000004bU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[46] = {{
        0x0000006cU, 0x0000004cU, 0x0000006cU, 0x0000004cU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[47] = {{
        0x0000003bU, 0x0000003aU, 0x0000003bU, 0x0000003aU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[48] = {{
        0x00000027U, 0x00000022U, 0x00000027U, 0x00000022U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[49] = {{
        0x00000060U, 0x0000007eU, 0x00000060U, 0x0000007eU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[50] = {{
        0x0000ffe1U, 0x00000000U, 0x0000ffe1U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[51] = {{
        0x0000005cU, 0x0000007cU, 0x0000005cU, 0x0000007cU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[52] = {{
        0x0000007aU, 0x0000005aU, 0x0000007aU, 0x0000005aU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[53] = {{
        0x00000078U, 0x00000058U, 0x00000078U, 0x00000058U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[54] = {{
        0x00000063U, 0x00000043U, 0x00000063U, 0x00000043U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[55] = {{
        0x00000076U, 0x00000056U, 0x00000076U, 0x00000056U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[56] = {{
        0x00000062U, 0x00000042U, 0x00000062U, 0x00000042U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[57] = {{
        0x0000006eU, 0x0000004eU, 0x0000006eU, 0x0000004eU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[58] = {{
        0x0000006dU, 0x0000004dU, 0x0000006dU, 0x0000004dU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[59] = {{
        0x0000002cU, 0x0000003cU, 0x0000002cU, 0x0000003cU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[60] = {{
        0x0000002eU, 0x0000003eU, 0x0000002eU, 0x0000003eU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[61] = {{
        0x0000002fU, 0x0000003fU, 0x0000002fU, 0x0000003fU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[62] = {{
        0x0000ffe2U, 0x00000000U, 0x0000ffe2U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[63] = {{
        0x0000ffaaU, 0x0000ffaaU, 0x0000ffaaU, 0x0000ffaaU, 0x0000ffaaU, 0x0000ffaaU, 0x1008fe21U
    }};
    result[64] = {{
        0x0000ffe9U, 0x00000000U, 0x0000ffe9U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[65] = {{
        0x00000020U, 0x00000000U, 0x00000020U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[66] = {{
        0x0000ffe5U, 0x00000000U, 0x0000ffe5U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[67] = {{
        0x0000ffbeU, 0x0000ffbeU, 0x0000ffbeU, 0x0000ffbeU, 0x0000ffbeU, 0x0000ffbeU, 0x1008fe01U
    }};
    result[68] = {{
        0x0000ffbfU, 0x0000ffbfU, 0x0000ffbfU, 0x0000ffbfU, 0x0000ffbfU, 0x0000ffbfU, 0x1008fe02U
    }};
    result[69] = {{
        0x0000ffc0U, 0x0000ffc0U, 0x0000ffc0U, 0x0000ffc0U, 0x0000ffc0U, 0x0000ffc0U, 0x1008fe03U
    }};
    result[70] = {{
        0x0000ffc1U, 0x0000ffc1U, 0x0000ffc1U, 0x0000ffc1U, 0x0000ffc1U, 0x0000ffc1U, 0x1008fe04U
    }};
    result[71] = {{
        0x0000ffc2U, 0x0000ffc2U, 0x0000ffc2U, 0x0000ffc2U, 0x0000ffc2U, 0x0000ffc2U, 0x1008fe05U
    }};
    result[72] = {{
        0x0000ffc3U, 0x0000ffc3U, 0x0000ffc3U, 0x0000ffc3U, 0x0000ffc3U, 0x0000ffc3U, 0x1008fe06U
    }};
    result[73] = {{
        0x0000ffc4U, 0x0000ffc4U, 0x0000ffc4U, 0x0000ffc4U, 0x0000ffc4U, 0x0000ffc4U, 0x1008fe07U
    }};
    result[74] = {{
        0x0000ffc5U, 0x0000ffc5U, 0x0000ffc5U, 0x0000ffc5U, 0x0000ffc5U, 0x0000ffc5U, 0x1008fe08U
    }};
    result[75] = {{
        0x0000ffc6U, 0x0000ffc6U, 0x0000ffc6U, 0x0000ffc6U, 0x0000ffc6U, 0x0000ffc6U, 0x1008fe09U
    }};
    result[76] = {{
        0x0000ffc7U, 0x0000ffc7U, 0x0000ffc7U, 0x0000ffc7U, 0x0000ffc7U, 0x0000ffc7U, 0x1008fe0aU
    }};
    result[77] = {{
        0x0000ff7fU, 0x00000000U, 0x0000ff7fU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[78] = {{
        0x0000ff14U, 0x00000000U, 0x0000ff14U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[79] = {{
        0x0000ff95U, 0x0000ffb7U, 0x0000ff95U, 0x0000ffb7U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[80] = {{
        0x0000ff97U, 0x0000ffb8U, 0x0000ff97U, 0x0000ffb8U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[81] = {{
        0x0000ff9aU, 0x0000ffb9U, 0x0000ff9aU, 0x0000ffb9U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[82] = {{
        0x0000ffadU, 0x0000ffadU, 0x0000ffadU, 0x0000ffadU, 0x0000ffadU, 0x0000ffadU, 0x1008fe23U
    }};
    result[83] = {{
        0x0000ff96U, 0x0000ffb4U, 0x0000ff96U, 0x0000ffb4U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[84] = {{
        0x0000ff9dU, 0x0000ffb5U, 0x0000ff9dU, 0x0000ffb5U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[85] = {{
        0x0000ff98U, 0x0000ffb6U, 0x0000ff98U, 0x0000ffb6U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[86] = {{
        0x0000ffabU, 0x0000ffabU, 0x0000ffabU, 0x0000ffabU, 0x0000ffabU, 0x0000ffabU, 0x1008fe22U
    }};
    result[87] = {{
        0x0000ff9cU, 0x0000ffb1U, 0x0000ff9cU, 0x0000ffb1U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[88] = {{
        0x0000ff99U, 0x0000ffb2U, 0x0000ff99U, 0x0000ffb2U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[89] = {{
        0x0000ff9bU, 0x0000ffb3U, 0x0000ff9bU, 0x0000ffb3U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[90] = {{
        0x0000ff9eU, 0x0000ffb0U, 0x0000ff9eU, 0x0000ffb0U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[91] = {{
        0x0000ff9fU, 0x0000ffaeU, 0x0000ff9fU, 0x0000ffaeU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[92] = {{
        0x0000fe03U, 0x00000000U, 0x0000fe03U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[94] = {{
        0x0000003cU, 0x0000003eU, 0x0000003cU, 0x0000003eU, 0x0000007cU, 0x000000a6U, 0x0000007cU
    }};
    result[95] = {{
        0x0000ffc8U, 0x0000ffc8U, 0x0000ffc8U, 0x0000ffc8U, 0x0000ffc8U, 0x0000ffc8U, 0x1008fe0bU
    }};
    result[96] = {{
        0x0000ffc9U, 0x0000ffc9U, 0x0000ffc9U, 0x0000ffc9U, 0x0000ffc9U, 0x0000ffc9U, 0x1008fe0cU
    }};
    result[98] = {{
        0x0000ff26U, 0x00000000U, 0x0000ff26U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[99] = {{
        0x0000ff25U, 0x00000000U, 0x0000ff25U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[100] = {{
        0x0000ff23U, 0x00000000U, 0x0000ff23U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[101] = {{
        0x0000ff27U, 0x00000000U, 0x0000ff27U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[102] = {{
        0x0000ff22U, 0x00000000U, 0x0000ff22U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[104] = {{
        0x0000ff8dU, 0x00000000U, 0x0000ff8dU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[105] = {{
        0x0000ffe4U, 0x00000000U, 0x0000ffe4U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[106] = {{
        0x0000ffafU, 0x0000ffafU, 0x0000ffafU, 0x0000ffafU, 0x0000ffafU, 0x0000ffafU, 0x1008fe20U
    }};
    result[107] = {{
        0x0000ff61U, 0x0000ff15U, 0x0000ff61U, 0x0000ff15U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[108] = {{
        0x0000ffeaU, 0x00000000U, 0x0000ffeaU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[110] = {{
        0x0000ff50U, 0x00000000U, 0x0000ff50U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[111] = {{
        0x0000ff52U, 0x00000000U, 0x0000ff52U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[112] = {{
        0x0000ff55U, 0x00000000U, 0x0000ff55U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[113] = {{
        0x0000ff51U, 0x00000000U, 0x0000ff51U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[114] = {{
        0x0000ff53U, 0x00000000U, 0x0000ff53U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[115] = {{
        0x0000ff57U, 0x00000000U, 0x0000ff57U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[116] = {{
        0x0000ff54U, 0x00000000U, 0x0000ff54U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[117] = {{
        0x0000ff56U, 0x00000000U, 0x0000ff56U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[118] = {{
        0x0000ff63U, 0x00000000U, 0x0000ff63U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[119] = {{
        0x0000ffffU, 0x00000000U, 0x0000ffffU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[125] = {{
        0x0000ffbdU, 0x00000000U, 0x0000ffbdU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[127] = {{
        0x0000ff13U, 0x0000ff6bU, 0x0000ff13U, 0x0000ff6bU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[129] = {{
        0x0000ffaeU, 0x0000ffaeU, 0x0000ffaeU, 0x0000ffaeU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[130] = {{
        0x0000ff31U, 0x00000000U, 0x0000ff31U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[131] = {{
        0x0000ff34U, 0x00000000U, 0x0000ff34U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[133] = {{
        0x0000ffebU, 0x00000000U, 0x0000ffebU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[134] = {{
        0x0000ffecU, 0x00000000U, 0x0000ffecU, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[135] = {{
        0x0000ff67U, 0x00000000U, 0x0000ff67U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[203] = {{
        0x0000fe11U, 0x00000000U, 0x0000fe11U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[204] = {{
        0x00000000U, 0x0000ffe9U, 0x00000000U, 0x0000ffe9U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[205] = {{
        0x00000000U, 0x0000ffe7U, 0x00000000U, 0x0000ffe7U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[206] = {{
        0x00000000U, 0x0000ffebU, 0x00000000U, 0x0000ffebU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[207] = {{
        0x00000000U, 0x0000ffedU, 0x00000000U, 0x0000ffedU, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[235] = {{
        0x1008ff59U, 0x00000000U, 0x1008ff59U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[236] = {{
        0x1008ff04U, 0x00000000U, 0x1008ff04U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[237] = {{
        0x1008ff06U, 0x00000000U, 0x1008ff06U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    result[238] = {{
        0x1008ff05U, 0x00000000U, 0x1008ff05U, 0x00000000U, 0x00000000U, 0x00000000U, 0x00000000U
    }};
    return result;
}();

inline constexpr std::size_t keys_per_modifier = 4;
inline constexpr std::array<std::uint8_t, 32> core_modifier_map{{
    50, 62, 0, 0, 66, 0, 0, 0,
    37, 105, 0, 0, 64, 108, 204, 205,
    77, 0, 0, 0, 203, 0, 0, 0,
    133, 134, 206, 0, 92, 0, 0, 0
}};
inline constexpr std::array<std::uint8_t, 32> default_auto_repeats{{
    0, 255, 255, 255, 223, 255, 251, 191,
    250, 223, 255, 239, 255, 237, 255, 255,
    159, 255, 255, 255, 255, 255, 255, 255,
    255, 247, 255, 255, 255, 255, 255, 255
}};
inline constexpr bool default_global_auto_repeat = true;
inline constexpr std::uint8_t default_key_click_percent = 0;
inline constexpr std::uint8_t default_bell_percent = 50;
inline constexpr std::uint16_t default_bell_pitch = 400;
inline constexpr std::uint16_t default_bell_duration = 100;
inline constexpr std::int16_t default_pointer_acceleration_numerator = 2;
inline constexpr std::int16_t default_pointer_acceleration_denominator = 1;
inline constexpr std::int16_t default_pointer_threshold = 4;
inline constexpr std::array<std::uint8_t, 10> default_pointer_map{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};

} // namespace xmin::server

#endif
