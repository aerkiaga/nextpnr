/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@clifford.at>
 *  Copyright (C) 2018  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include "bitstream.h"
#include <vector>

NEXTPNR_NAMESPACE_BEGIN

inline TileType tile_at(const Chip &chip, int x, int y)
{
    return chip.chip_info.tile_grid[y * chip.chip_info.width + x];
}

const ConfigEntryPOD &find_config(const TileInfoPOD &tile,
                                  const std::string &name)
{
    for (int i = 0; i < tile.num_config_entries; i++) {
        if (std::string(tile.entries[i].name) == name) {
            return tile.entries[i];
        }
    }
    assert(false);
}

std::tuple<int8_t, int8_t, int8_t> get_ieren(const BitstreamInfoPOD &bi,
                                             int8_t x, int8_t y, int8_t z)
{
    for (int i = 0; i < bi.num_ierens; i++) {
        auto ie = bi.ierens[i];
        if (ie.iox == x && ie.ioy == y && ie.ioz == z) {
            return std::make_tuple(ie.ierx, ie.iery, ie.ierz);
        }
    }
    // No pin at this location
    return std::make_tuple(-1, -1, -1);
};

void set_config(const TileInfoPOD &ti,
                std::vector<std::vector<int8_t>> &tile_cfg,
                const std::string &name, bool value, int index = -1)
{
    const ConfigEntryPOD &cfg = find_config(ti, name);
    if (index == -1) {
        for (int i = 0; i < cfg.num_bits; i++) {
            int8_t &cbit = tile_cfg.at(cfg.bits[i].row).at(cfg.bits[i].col);
            cbit = value;
        }
    } else {
        int8_t &cbit = tile_cfg.at(cfg.bits[index].row).at(cfg.bits[index].col);
        cbit = value;
    }
}

void write_asc(const Design &design, std::ostream &out)
{
    const Chip &chip = design.chip;
    // [y][x][row][col]
    const ChipInfoPOD &ci = chip.chip_info;
    const BitstreamInfoPOD &bi = *ci.bits_info;
    std::vector<std::vector<std::vector<std::vector<int8_t>>>> config;
    config.resize(ci.height);
    for (int y = 0; y < ci.height; y++) {
        config.at(y).resize(ci.width);
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            int rows = bi.tiles_nonrouting[tile].rows;
            int cols = bi.tiles_nonrouting[tile].cols;
            config.at(y).at(x).resize(rows, std::vector<int8_t>(cols));
        }
    }
    out << ".comment from next-pnr" << std::endl;

    switch (chip.args.type) {
    case ChipArgs::LP384:
        out << ".device 384" << std::endl;
        break;
    case ChipArgs::HX1K:
    case ChipArgs::LP1K:
        out << ".device 1k" << std::endl;
        break;
    case ChipArgs::HX8K:
    case ChipArgs::LP8K:
        out << ".device 8k" << std::endl;
        break;
    case ChipArgs::UP5K:
        out << ".device 5k" << std::endl;
        break;
    default:
        assert(false);
    }
    // Set pips
    for (auto pip : chip.getPips()) {
        if (chip.pip_to_net[pip.index] != IdString()) {
            const PipInfoPOD &pi = ci.pip_data[pip.index];
            const SwitchInfoPOD &swi = bi.switches[pi.switch_index];
            for (int i = 0; i < swi.num_bits; i++) {
                bool val =
                        (pi.switch_mask & (1 << ((swi.num_bits - 1) - i))) != 0;
                int8_t &cbit = config.at(swi.y)
                                       .at(swi.x)
                                       .at(swi.cbits[i].row)
                                       .at(swi.cbits[i].col);
                if (bool(cbit) != 0)
                    assert(false);
                cbit = val;
            }
        }
    }
    // Set logic cell config
    for (auto cell : design.cells) {
        BelId bel = cell.second->bel;
        if (bel == BelId()) {
            std::cout << "Found unplaced cell " << cell.first
                      << " while generating bitstream!" << std::endl;
            continue;
        }
        const BelInfoPOD &beli = ci.bel_data[bel.index];
        int x = beli.x, y = beli.y, z = beli.z;
        if (cell.second->type == "ICESTORM_LC") {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_LOGIC];
            unsigned lut_init = std::stoi(cell.second->params["LUT_INIT"]);
            bool neg_clk = std::stoi(cell.second->params["NEG_CLK"]);
            bool dff_enable = std::stoi(cell.second->params["DFF_ENABLE"]);
            bool async_sr = std::stoi(cell.second->params["ASYNC_SR"]);
            bool set_noreset = std::stoi(cell.second->params["SET_NORESET"]);
            bool carry_enable = std::stoi(cell.second->params["CARRY_ENABLE"]);
            std::vector<bool> lc(20, false);
            // From arachne-pnr
            static std::vector<int> lut_perm = {
                    4, 14, 15, 5, 6, 16, 17, 7, 3, 13, 12, 2, 1, 11, 10, 0,
            };
            for (int i = 0; i < 16; i++) {
                if ((lut_init >> i) & 0x1)
                    lc.at(lut_perm.at(i)) = true;
            }
            lc.at(8) = carry_enable;
            lc.at(9) = dff_enable;
            lc.at(18) = set_noreset;
            lc.at(19) = async_sr;

            for (int i = 0; i < 20; i++)
                set_config(ti, config.at(y).at(x), "LC_" + std::to_string(z),
                           lc.at(i), i);
            set_config(ti, config.at(y).at(x), "NegClk", neg_clk);
        } else if (cell.second->type == "SB_IO") {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_IO];
            unsigned pin_type = std::stoi(cell.second->params["PIN_TYPE"]);
            bool neg_trigger = std::stoi(cell.second->params["NEG_TRIGGER"]);
            bool pullup = std::stoi(cell.second->params["PULLUP"]);
            for (int i = 0; i < 6; i++) {
                bool val = (pin_type >> i) & 0x01;
                set_config(ti, config.at(y).at(x),
                           "IOB_" + std::to_string(z) + ".PINTYPE_" +
                                   std::to_string(i),
                           val);
            }

            auto ieren = get_ieren(bi, x, y, z);
            int iex, iey, iez;
            std::tie(iex, iey, iez) = ieren;
            assert(iez != -1);

            bool input_en = false;
            if ((chip.wire_to_net[chip.getWireBelPin(bel, PIN_D_IN_0).index] !=
                 IdString()) ||
                (chip.wire_to_net[chip.getWireBelPin(bel, PIN_D_IN_1).index] !=
                 IdString())) {
                input_en = true;
            }

            if (chip.args.type == ChipArgs::LP1K ||
                chip.args.type == ChipArgs::HX1K) {
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.IE_" + std::to_string(iez), !input_en);
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.REN_" + std::to_string(iez), !pullup);
            } else {
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.IE_" + std::to_string(iez), input_en);
                set_config(ti, config.at(iey).at(iex),
                           "IoCtrl.REN_" + std::to_string(iez), !pullup);
            }
        } else if (cell.second->type == "SB_GB") {
            // no cell config bits
        } else {
            assert(false);
        }
    }
    // Set config bits in unused IO
    for (auto bel : chip.getBels()) {
        if (chip.bel_to_cell[bel.index] == IdString() &&
            chip.getBelType(bel) == TYPE_SB_IO) {
            TileInfoPOD &ti = bi.tiles_nonrouting[TILE_IO];
            const BelInfoPOD &beli = ci.bel_data[bel.index];
            int x = beli.x, y = beli.y, z = beli.z;
            auto ieren = get_ieren(bi, x, y, z);
            int iex, iey, iez;
            std::tie(iex, iey, iez) = ieren;
            if (iez != -1) {
                if (chip.args.type == ChipArgs::LP1K ||
                    chip.args.type == ChipArgs::HX1K) {
                    set_config(ti, config.at(iey).at(iex),
                               "IoCtrl.IE_" + std::to_string(iez), true);
                    set_config(ti, config.at(iey).at(iex),
                               "IoCtrl.REN_" + std::to_string(iez), false);
                }
            }
        }
    }

    // Set other config bits
    for (int y = 0; y < ci.height; y++) {
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            TileInfoPOD &ti = bi.tiles_nonrouting[tile];

            // disable RAM to stop icebox_vlog crashing (FIXME)
            if ((tile == TILE_RAMB) && (chip.args.type == ChipArgs::LP1K ||
                                        chip.args.type == ChipArgs::HX1K)) {
                set_config(ti, config.at(y).at(x), "RamConfig.PowerUp", true);
            }

            // set all ColBufCtrl bits (FIXME)
            bool setColBufCtrl = true;
            if (chip.args.type == ChipArgs::LP1K ||
                chip.args.type == ChipArgs::HX1K) {
                if (tile == TILE_RAMB || tile == TILE_RAMT) {
                    setColBufCtrl = (y == 3 || y == 5 || y == 11 || y == 13);
                } else {
                    setColBufCtrl = (y == 4 || y == 5 || y == 12 || y == 13);
                }
            } else if (chip.args.type == ChipArgs::LP8K ||
                       chip.args.type == ChipArgs::HX8K) {
                setColBufCtrl = (y == 8 || y == 9 || y == 24 || y == 25);
            } else if (chip.args.type == ChipArgs::UP5K) {
                if (tile == TILE_LOGIC) {
                    setColBufCtrl = (y == 4 || y == 5 || y == 14 || y == 15 ||
                                     y == 26 || y == 27);
                } else {
                    setColBufCtrl = false;
                }
            }
            if (setColBufCtrl) {
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_0",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_1",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_2",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_3",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_4",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_5",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_6",
                           true);
                set_config(ti, config.at(y).at(x), "ColBufCtrl.glb_netwk_7",
                           true);
            }
        }
    }

    // Write config out
    for (int y = 0; y < ci.height; y++) {
        for (int x = 0; x < ci.width; x++) {
            TileType tile = tile_at(chip, x, y);
            if (tile == TILE_NONE)
                continue;
            switch (tile) {
            case TILE_LOGIC:
                out << ".logic_tile";
                break;
            case TILE_IO:
                out << ".io_tile";
                break;
            case TILE_RAMB:
                out << ".ramb_tile";
                break;
            case TILE_RAMT:
                out << ".ramt_tile";
                break;
            default:
                assert(false);
            }
            out << " " << x << " " << y << std::endl;
            for (auto row : config.at(y).at(x)) {
                for (auto col : row) {
                    if (col == 1)
                        out << "1";
                    else
                        out << "0";
                }
                out << std::endl;
            }
            out << std::endl;
        }
    }
}

NEXTPNR_NAMESPACE_END
