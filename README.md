# psg_compressor

This software is intended to pack PSG files. It is register dump for music chip AY-3-8910.

psg_packer 	    - packer for PC.
fast_psg_player.asm - music player for ZX spectrum for compression levels [0..3].
l4_psg_player.asm   - music player for ZX spectrum for compression levels [4..5].

Recomended compression levels:
	1 - for fast unpack (unpack speed <=799t).
	4 - for beter compression (unpack speed <=930t).
