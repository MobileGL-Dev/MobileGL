"""Download the fonts the paper-cutout video uses into ./fonts (all SIL OFL 1.1). Run once before rendering."""
import io
import os
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
GF = 'https://github.com/google/fonts/raw/main/ofl/'
FONTS = {
    'Fredoka.ttf': GF + 'fredoka/Fredoka%5Bwdth,wght%5D.ttf',
    'PatrickHand.ttf': GF + 'patrickhand/PatrickHand-Regular.ttf',
    'ZCOOLKuaiLe.ttf': GF + 'zcoolkuaile/ZCOOLKuaiLe-Regular.ttf',
    'LXGWWenKai.ttf': 'https://github.com/lxgw/LxgwWenKai/releases/download/v1.522/LXGWWenKai-Regular.ttf',
}
CASCADIA = 'https://github.com/microsoft/cascadia-code/releases/download/v2407.24/CascadiaCode-2407.24.zip'


def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


if __name__ == '__main__':
    out = os.path.join(HERE, 'fonts')
    os.makedirs(out, exist_ok=True)
    for name, url in FONTS.items():
        path = os.path.join(out, name)
        if not os.path.exists(path):
            print('fetching', name)
            with open(path, 'wb') as f:
                f.write(fetch(url))
    path = os.path.join(out, 'CascadiaMono.ttf')
    if not os.path.exists(path):
        print('fetching CascadiaMono.ttf')
        with zipfile.ZipFile(io.BytesIO(fetch(CASCADIA))) as z, open(path, 'wb') as f:
            f.write(z.read('ttf/CascadiaMono.ttf'))
    print('fonts ready in', out)
