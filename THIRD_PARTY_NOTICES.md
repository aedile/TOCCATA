# Third-party code

## vecx 6809 core (GPL-3.0) — this one is contagious

`core/e6809.c` and `core/e6809.h` are the MC6809 emulator from
[vecx](https://github.com/jhawthorn/vecx), copyright Valavan Manohararajah and
the vecx contributors, licensed GPL-3.0 (see `LICENSES/GPL-3.0.txt`).

**Because a build links this core in, the built firmware and host harness are
GPL-3.0 as a whole.** Replace that one file with a permissively licensed 6809
and the rest of the project is 0BSD.

Modified for this project: the opcode fetch is split out from the ordinary read
so the board can decrypt it. Gyruss's sub-CPU is a KONAMI-1, a 6809 whose
opcodes — and only its opcodes — are scrambled.

## Marat Fayzullin's Z80 emulator (non-commercial)

`core/z80/` is the portable Z80 emulator by Marat Fayzullin, copyright (C)
Marat Fayzullin 1994-2007, from http://fms.komkon.org/EMUL8/. Its terms, from
the source headers: "You are not allowed to distribute this software
commercially. Please, notify me, if you make any changes to this file." The
files are unmodified; the project builds them with `LSB_FIRST` defined.

## MAME (BSD-3-Clause)

The machine model in `core/gyruss.c` (the four processors' memory maps, the
latches between them, the LS90 tempo counter read through the third sound chip,
and the interrupt masks) and the video in `core/gyruss_video.c` (the tile and
sprite layouts, the two colour lookup PROMs and the two-pass tilemap) are
written from MAME's `src/mame/konami/gyruss.cpp`. The KONAMI-1 decryption is
from `src/mame/konami/konami1.cpp`, copyright Olivier Galibert. Copyright the
MAME team; used under the BSD-3-Clause license:

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

## The MCS-48 core and the AY-3-8910

`core/mcs48.h` and `core/ay8910.c` were written for this project's family of
medals and are covered by the project licence. The AY's sixteen-step volume
table is MAME's measured one; its dividers are from the General Instrument
datasheet.

## The discrete filter network

The real board runs each sound chip's output through an RC network the program
switches with the chips' port B, and mixes the five of them through a resistor
ladder. That is not modelled here: the channels are summed flat.

## ROMs

No game ROMs are included in this repository, and none ever will be. Supplying
them is up to whoever builds the thing.
