"""Download the fonts the hype reel uses into ./fonts (SIL OFL 1.1, Google Fonts). Run once before rendering."""
import os
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
GF = 'https://github.com/google/fonts/raw/main/ofl/'
FONTS = {
    'Archivo[wdth,wght].ttf': GF + 'archivo/Archivo%5Bwdth,wght%5D.ttf',
    'Archivo-Italic[wdth,wght].ttf': GF + 'archivo/Archivo-Italic%5Bwdth,wght%5D.ttf',
    'JetBrainsMono[wght].ttf': GF + 'jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf',
}

if __name__ == '__main__':
    out = os.path.join(HERE, 'fonts')
    os.makedirs(out, exist_ok=True)
    for name, url in FONTS.items():
        path = os.path.join(out, name)
        if not os.path.exists(path):
            print('fetching', name)
            with urllib.request.urlopen(url) as r, open(path, 'wb') as f:
                f.write(r.read())
    print('fonts ready in', out)
