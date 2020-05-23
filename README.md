# ColorFiller

An open source homebrew recreation of [FlowFree®](https://www.bigduckgames.com/flowfree) in C++ for the 3DS !

## Levels

The levels can be created using an editor (soon™), or by dumping the `levelpack_*.txt` from the `assets` folder of the game APK.  
Simply use an apk dumper application on your phone (there are many) to get the file, then open it with [7-zip](https://www.7-zip.org/) or your favourite archive extractor.  
Put the level packs you want in a `levels` folder in the same directory as the `convert_level_packs.py` file, then run said file (requires [Python 3.6+](https://www.python.org/)).  
You will get a file named `levels.zip` which you should put on your 3DS' SD card, at the path specified by the `levels_path` settings of your configuration file (the default is `sd:/3ds/ColorFillerLevels.zip`).  
You are now ready to play the game! Do note that changing your levels file can invalidate your save file, so I recommend making backups.

## License

This version of the game is licensed under the GPLv3.

## Credits

Many thanks to the [libctru](https://github.com/smealum/ctrulib/), [citro3d](https://github.com/fincs/citro3d/), and [citro2d](https://github.com/devkitPro/citro2d/) maintainers and contributors for the amazing libraries.  
[Big Duck Games](https://www.bigduckgames.com/) for the original game, check them out and buy the extra packs! They deserve it, and playing on phone is a much better experience!  
[icons8](https://icons8.com/) for the `reset.png`, `go_back.png`, and `scale.png` images in the `assets` folder!
