# assets/fonts — shipped typefaces

Every face here is **SIL Open Font License 1.1**, sourced from the
[google/fonts](https://github.com/google/fonts) repository (`ofl/<family>/`).
Each family ships its own unmodified `OFL.txt` beside the face, which is the
license's redistribution requirement.

Unlike `assets/textures` and `assets/models`, these are **committed source** —
they are small, they are not derived, and the game hard-falls-back to Consolas
without them. A fresh clone or a new worktree gets them for free; they are not
part of the gitignored-asset provisioning dance.

## Provenance

| Family | Face(s) | Source | Modified? |
|---|---|---|---|
| Alegreya | Alegreya-Regular.ttf | `ofl/alegreya/Alegreya[wght].ttf` | instanced wght=400 |
| Bitter | Bitter-Regular.ttf | `ofl/bitter/Bitter[wght].ttf` | instanced wght=400 |
| Cinzel | Cinzel-Regular.ttf | `ofl/cinzel/Cinzel[wght].ttf` | instanced wght=400 |
| GentiumBookPlus | GentiumBookPlus-Regular.ttf | `ofl/gentiumbookplus/` | no — shipped static |
| GrenzeGotisch | GrenzeGotisch-Regular.ttf | `ofl/grenzegotisch/GrenzeGotisch[wght].ttf` | instanced wght=400 |
| IMFellEnglish | IMFeENrm28P.ttf, IMFeENit28P.ttf | `ofl/imfellenglish/` | no — shipped static |
| JetBrainsMono | JetBrainsMono-Regular.ttf | `ofl/jetbrainsmono/JetBrainsMono[wght].ttf` | instanced wght=400 |
| Marcellus | Marcellus-Regular.ttf | `ofl/marcellus/` | no — shipped static |
| PetitFormalScript | PetitFormalScript-Regular.ttf | `ofl/petitformalscript/` | no — shipped static |
| Spectral | Spectral-Regular.ttf | `ofl/spectral/` | no — shipped static |

## Why the variable fonts were instanced

stb_truetype has no font-variations support
([nothings/stb#509](https://github.com/nothings/stb/issues/509)), so it renders a
variable font's **default instance** and ignores `gvar` entirely. That is not
always Regular:

```
cinzel         wght  min 400  default 400  max 900
grenzegotisch  wght  min 100  default 400  max 900
alegreya       wght  min 400  default 400  max 900
bitter         wght  min 100  default 100  max 900   <-- default is THIN
jetbrainsmono  wght  min 100  default 400  max 800
```

**Bitter's default instance is Thin (100).** Shipping the variable file would
have rendered Bitter as a spindly hairline face at every size — and the audition
would have rejected it for a fault that isn't its own. Instancing each variable
font to `wght=400` removes the ambiguity and drops the now-dead variation tables
(Alegreya 425 KB -> 268 KB, Cinzel 125 KB -> 77 KB).

Reproduce with fontTools:

```
python -m fontTools.varLib.instancer <Family>[wght].ttf wght=400 --update-name-table
```

## Reserved Font Names

The OFL's RFN clause restricts using a reserved name for a **modified** version.
Cross-referencing what we modified against what declares an RFN:

| Family | RFN declared | Modified by us | Action |
|---|---|---|---|
| Bitter | "Bitter Pro" | yes (instanced) | see below |
| GentiumBookPlus | "Gentium", "SIL" | no | none needed |
| Marcellus | "Marcellus" | no | none needed |
| PetitFormalScript | "Petit Formal Script" | no | none needed |
| Alegreya, Cinzel, GrenzeGotisch, IMFellEnglish, JetBrainsMono, Spectral | none | — | none needed |

**Bitter is the only face that is both modified and RFN-bearing.** Its reserved
name is "Bitter Pro"; our instance identifies as "Bitter Regular", so the
reserved string is not in use. That reading is almost certainly fine, but it is
an interpretation — so if Bitter wins the body-face audition, confirm it before
release (or substitute a genuine upstream static). For the faces that do not
ship, the question is moot.

## Latin-1 coverage

`Font::Rebake` pre-warms 32..255, so every shipped face was checked across
printable Latin-1 (0x20-0x7E, 0xA0-0xFF). All ten families cover it completely
except for `U+00AD SOFT HYPHEN` (missing in Bitter and Spectral) — an invisible
formatting control that no game text uses.

## Optical size

Faces differ enough in x-height that the same pixel size reads as a different
size on screen. Per 1000 upem:

```
face                            x-height   cap
IMFeENit28P                          445   699
GrenzeGotisch-Regular                448   603
Spectral-Regular                     450   660
Alegreya-Regular                     452   637
GentiumBookPlus-Regular              454   615
IMFeENrm28P                          470   692
Cinzel-Regular                       500   700
Marcellus-Regular                    500   700
Bitter-Regular                       528   698
JetBrainsMono-Regular                550   730
PetitFormalScript-Regular            579   786
```

That is a ~30% spread between the extremes. Petit Formal Script set at 17px has
lowercase ~30% taller than IM Fell italic at the same 17px. This is what the
per-role `scale` knob in `fonts.cat` exists to compensate for — see
`docs/fonts.md`. As a starting point, a role's scale can be seeded as
`(target x-height) / (face x-height)` against whatever face the layout was
tuned on.
