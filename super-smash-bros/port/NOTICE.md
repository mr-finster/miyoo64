# Third-party code in port/

- `decomp/`: JRickey/ssb-decomp-re @ `2898d4d94e998ddb4e971addf7f6d85a39792f3d` (the port fork of
  VetriTheRetri/ssb-decomp-re pinned by BattleShip), code and headers only. `src/relocData/`
  (game data transcribed as C), `src/ovl8/` and `.c` of `src/db/` are not imported. No licence file upstream.
- `hh/` headers and sources marked "BattleShip": JRickey/BattleShip @ `c581ade`, MIT licence:

      MIT License
      Copyright (c) 2026 JRickey and contributors
      Permission is hereby granted, free of charge, to any person obtaining a copy of this software
      and associated documentation files (the "Software"), to deal in the Software without
      restriction, including without limitation the rights to use, copy, modify, merge, publish,
      distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
      Software is furnished to do so, subject to the following conditions:
      The above copyright notice and this permission notice shall be included in all copies or
      substantial portions of the Software.
      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
      BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
      NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
      DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Taken so far: port_log.h, port.h, fighter_registry.h, port_aobj_fixup.h, bridge/*.h, audio/*.h,
  hh/include/reloc_data.h (BattleShip include/reloc_data.us.h: file IDs and offsets, numbers only),
  and the C half of enhancements/enhancements.h.
- `hh/bs/mixer.c`: BattleShip's mixer, itself derived from the Starship (Star Fox 64) PC port,
  HarbourMasters/Starship, MIT licence:

      MIT License
      Copyright (c) The Harbour Masters
      Permission is hereby granted, free of charge, to any person obtaining a copy of this software
      and associated documentation files (the "Software"), to deal in the Software without
      restriction, including without limitation the rights to use, copy, modify, merge, publish,
      distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
      Software is furnished to do so, subject to the following conditions:
      The above copyright notice and this permission notice shall be included in all copies or
      substantial portions of the Software.
      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
      BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
      NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
      DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

- `hh/gfx/`: the MK64/OoT port kit (`mario-kart-64/` and `ocarina-of-time/` in this repository).

This notice lives in the repo only: the Miyoo zip carries no text files.
- `tools/creditsTextConverter.py`: from VetriTheRetri/ssb-decomp-re.
