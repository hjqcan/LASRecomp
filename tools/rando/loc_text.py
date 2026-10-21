#!/usr/bin/env python3
"""Our own message texts in every language the game has (the Escape Bottle, the Hint Sheet): the sentences are
ours, written here per language; names of items and places come from the game's own message
files of that language at generation time (UI.msbt ItemName_*, Glossary.msbt, Place.msbt), never stored here.

    lang_of(rel)                       'regionEU/EUde/message/System.msbt' -> 'de' (LANGS)
    Names(out_dir, romfs, rel)         the game's own names in that language: .item(las item), .place(label), ...
    wrap(segments, wait, lang)         [(str | bytes, red)] -> message bytes, lines and pages fitted to the box
    T[key][lang]                       our sentences (English ones live with their modules)

Register follows the game's own text in each language: European French "vous", American French "tu", European
Spanish "Has conseguido", American Spanish "Obtuviste".
"""
import os
import re

import keysanity_text as kt
from msbt import Msbt

# message folder -> our language code ('*_Validation' folders: the same language)
FOLDERS = {'USen': 'en', 'EUen': 'en', 'EUde': 'de', 'EUfr': 'fr', 'USfr': 'fr_us', 'EUes': 'es', 'USes': 'es_us',
           'EUit': 'it', 'EUnl': 'nl', 'EUru': 'ru', 'JPja': 'ja', 'KRkr': 'ko', 'CNch': 'zh_cn', 'TWch': 'zh_tw'}
LANGS = sorted(set(FOLDERS.values()))
# characters per line of the message box (two lines a page): Latin / Cyrillic like the English hints (34, a little
# more for the longer words), full-width scripts about half (the game's own lines: 16-18)
LINE = {'en': 34, 'ja': 16, 'zh_cn': 16, 'zh_tw': 16, 'ko': 20}
CHAR_WRAP = {'zh_cn', 'zh_tw'}  # (no spaces between words: lines break between any two characters)
NO_LINE_START = set('！。，、）：？!?,.)」』')

RED = b'\x0e\x00\x00\x03\x00\x04\x00\xd9\x45\x6d\xff'  # colour tags: the game's item-name red / plain white
WHITE = b'\x0e\x00\x00\x03\x00\x04\x00\xed\xed\xed\xff'
PAGE = b'\x0e\x00\x00\x04\x00\x00\x00'


def lang_of(rel):
    folder = rel.replace('\\', '/').split('/')[1].replace('_Validation', '')
    return FOLDERS.get(folder, 'en')


def _clean(raw):
    """A game message as an inline name: its text and tags (ruby...) up to the end, no page / wait tags."""
    return b''.join(r for k, g, r in kt._tokens(raw or b'') if g not in (kt.WAIT, kt.PAGE))


def _shown(raw):
    return sum(len(r.decode('utf-8', 'replace')) for k, g, r in kt._tokens(raw) if k == 'text')


def wrap(segments, wait, lang, line=None):
    """[(text, red)] -> message bytes: our text (str) breaks between words (Chinese: between characters), a game
    name (bytes) never; lines of at most `line` characters, two per page."""
    limit = line or LINE.get(lang, 36)
    toks = []  # (bytes, shown chars, red, space before, may break before)
    space = False  # (a space still to come before the next word: it may sit at the end of the segment before)
    for text, red in segments:
        if isinstance(text, bytes):
            toks.append((text, _shown(text), red, space, space or not toks))
            space = False
            continue
        if lang in CHAR_WRAP:
            for ch in text:
                toks.append((ch.encode(), 1, red, False, ch not in NO_LINE_START))
            continue
        for p in re.split(r'( +)', text):
            if not p:
                continue
            if p.startswith(' '):
                space = True
                continue
            toks.append((p.encode(), len(p), red, space, space or not toks))
            space = False
    lines, cur, width = [], [], 0
    for t in toks:
        add = t[1] + (1 if t[3] and cur else 0)
        if cur and width + add > limit and t[4]:
            lines.append(cur)
            cur, width = [], 0
            add = t[1]
        cur.append(t)
        width += add
    if cur:
        lines.append(cur)
    out, red = b'', False
    for i, ln in enumerate(lines):
        if i:
            out += wait + PAGE if i % 2 == 0 else b'\n'
        for j, (b, _, r, sp, _) in enumerate(ln):
            if j and sp:
                out += b' '
            if r != red:
                out += RED if r else WHITE
                red = r
            out += b
    if red:
        out += WHITE
    return out


def pages(page_segments, wait, lang):
    """Several pages of segments, each wrapped on its own -> one message (ends with the wait and the terminator)."""
    return (wait + PAGE).join(wrap(p, wait, lang) for p in page_segments) + wait + b'\0'


# ---------------------------------------------------------------- the game's names, per language
# LAS item -> UI.msbt ItemName_<label> (or Glossary.msbt glossary_<label> when it starts with 'g:')
ITEM_LABEL = {
    'sword': 'Sword1', 'shield': 'Shield', 'bracelet': 'Bracelet1', 'boots': 'PegasusBoots', 'flippers': 'Flippers',
    'feather': 'Feather', 'shovel': 'Shovel', 'hookshot': 'HookShot', 'bow': 'Bow', 'rod': 'MagicRod',
    'boomerang': 'Boomerang', 'ocarina': 'Ocarina', 'song-ballad': 'WindFish', 'song-mambo': 'Mambo', 'song-soul': 'Soul',
    'yoshi-doll': 'YoshiDoll', 'ribbon': 'Ribbon', 'dog-food': 'DogFood', 'bananas': 'Bananas', 'stick': 'Stick',
    'honeycomb': 'Honeycomb', 'pineapple': 'Pineapple', 'hibiscus': 'Hibiscus', 'letter': 'Letter', 'broom': 'Broom',
    'fishing-hook': 'FishingHook', 'necklace': 'PinkBra', 'mermaid-scale': 'MermaidsScale',
    'magnifying-lens': 'MagnifyingLens', 'mushroom': 'SleepyMushroom', 'tail-key': 'TailKey', 'slime-key': 'SlimeKey',
    'angler-key': 'AnglerKey', 'face-key': 'FaceKey', 'bird-key': 'BirdKey', 'full-moon-cello': 'Violin',
    'conch-horn': 'Horn', 'sea-lilys-bell': 'Bell', 'surf-harp': 'Harp', 'wind-marimba': 'Marimba',
    'coral-triangle': 'Triangle', 'evening-calm-organ': 'Organ', 'thunder-drum': 'Drum', 'shell-sensor': 'ShellRader',
    'red-tunic': 'WearRed', 'blue-tunic': 'WearBlue', 'bomb': 'Bomb', 'powder': 'Powder',
    'chamber-stone': 'PanelDungeonPiece', 'seashell': 'g:Item34', 'golden-leaf': 'g:Item36', 'heart-piece': 'g:Item40',
    'heart-container': 'g:Item41', 'bottle': 'g:Item77', 'medicine': 'g:Item35',
    'map': 'g:Item16', 'compass': 'g:Item17', 'key': 'g:Item18', 'nightmare-key': 'g:Item19', 'stone-beak': 'g:Item45'}
# Field areas of the Hint Sheet (hint_sheet.AREA keys) -> Place.msbt labels
AREA_LABEL = {
    'mabe-village': 'MabeVillage', 'toronbo-shores': 'ToronboShores', 'mysterious-woods': 'Forest',
    'koholint-prairie': 'KoholintPrairie', 'tabahl-wasteland': 'TabahlWasteland', 'ukuku-prairie': 'UkukuPrairie',
    'sign-maze': 'SignpostMaze', 'goponga-swamp': 'GopongaSwamp', 'taltal-heights': 'TalTalHeights',
    'marthas-bay': 'MarthasBay', 'kanalet-castle': 'KanaletCastle', 'pothole-field': 'PotholeField',
    'animal-village': 'AnimalVillage', 'yarna-desert': 'YarnaDesert', 'ancient-ruins': 'SouthShrine',
    'rapids-ride': 'RapidsRide', 'taltal-mountains-east': 'TalTalMountain', 'taltal-mountains-west': 'TalTalMountain'}


def dungeon_label(dn):
    return 'ClothesDungeon_map' if dn == 'D0' else f'Lv{dn[1]}Dungeon_map'


class Names:
    """The game's own names in the language of System.msbt at `rel` (seed copies first, as the other text modules)."""

    def __init__(self, out_dir, romfs, rel):
        def load(name):
            r = rel.replace('System.msbt', name)
            for base in (os.path.join(out_dir, 'romfs'), romfs):
                p = os.path.join(base, r)
                if os.path.exists(p):
                    with open(p, 'rb') as f:
                        return Msbt(f.read()).texts
            return {}
        self.lang = lang_of(rel)
        self.ui, self.glossary, self.places = load('UI.msbt'), load('Glossary.msbt'), load('Place.msbt')

    def item(self, las_item):
        label = ITEM_LABEL.get(las_item)
        if not label:
            return None
        raw = self.glossary.get('glossary_' + label[2:]) if label.startswith('g:') else self.ui.get('ItemName_' + label)
        return _clean(raw) or None

    def place(self, label):
        return _clean(self.places.get(label)) or None


# ---------------------------------------------------------------- our sentences
# ('{name}' = the item's name, shown red; '{mabe}' = the game's name of Mabe Village)
T = {
    'escape': {  # the Escape Bottle's get text: three pages
        'de': ['Du hast die {name} erhalten! Mit ihr entkommst du jeder brenzligen Lage.',
               'Öffne sie in einem Labyrinth, um zum Eingang zurückzukehren!',
               'Öffne sie anderswo, um nach {mabe} heimzukehren!'],
        'fr': ["Vous avez obtenu la {name} ! Utilisez-la pour vous tirer d'affaire.",
               "Débouchez-la dans un donjon pour revenir à l'entrée !",
               'Débouchez-la ailleurs pour rentrer au {mabe} !'],
        'fr_us': ["Tu as obtenu la {name}! Utilise-la pour te tirer d'affaire!",
                  "Débouche-la dans un donjon pour revenir à l'entrée!",
                  'Débouche-la ailleurs pour rentrer au {mabe}!'],
        'es': ['¡Has conseguido la {name}! Úsala para salir de un apuro.',
               '¡Destápala en una mazmorra para volver a la entrada!',
               '¡Destápala en cualquier otro lugar para volver a {mabe}!'],
        'es_us': ['¡Obtuviste la {name}! Úsala para salir de apuros.',
                  '¡Destápala en una mazmorra para regresar a la entrada!',
                  '¡Destápala en cualquier otro lugar para regresar a {mabe}!'],
        'it': ['Hai ottenuto la {name}! Usala per toglierti dai guai.',
               "Stappala in un labirinto per tornare all'ingresso!",
               'Stappala altrove per tornare al {mabe}!'],
        'nl': ['Je hebt de {name}! Gebruik hem om uit de problemen te komen.',
               'Ontkurk hem in een kerker om terug te gaan naar de ingang!',
               'Ontkurk hem ergens anders om terug te keren naar {mabe}!'],
        'ru': ['Вы получили {name}! С ней можно выбраться из любой беды.',
               'Откройте ее в подземелье, чтобы вернуться ко входу!',
               'Откройте ее в любом другом месте, чтобы перенестись домой!'],
        'ja': ['{name}を 手に入れた！ ピンチの時に 使おう！',
               'ダンジョンで 使うと 入口に 戻れるぞ！',
               'それ以外の 場所で 使うと {mabe}に 帰れるぞ！'],
        'ko': ['{name}을 손에 넣었다! 위기에 빠졌을 때 사용하자!',
               '던전에서 사용하면 입구로 돌아갈 수 있다!',
               '그 밖의 장소에서 사용하면 {mabe}로 돌아갈 수 있다!'],
        'zh_cn': ['得到了{name}！遇到危险时就用它吧！', '在迷宫中打开，就能回到入口！', '在其他地方打开，就能回到{mabe}！'],
        'zh_tw': ['得到了{name}！遇到危險時就用它吧！', '在迷宮中打開，就能回到入口！', '在其他地方打開，就能回到{mabe}！'],
    },
    'escape-name': {'de': 'Fluchtflasche', 'fr': 'Fiole de fuite', 'fr_us': 'Fiole de fuite', 'es': 'botella de escape',
                    'es_us': 'botella de escape', 'it': 'bottiglia di fuga', 'nl': 'ontsnappingsfles',
                    'ru': 'бутылку побега', 'ja': 'エスケープボトル', 'ko': '탈출의 병', 'zh_cn': '逃脱之瓶',
                    'zh_tw': '逃脫之瓶'},
    'hint-get': {  # the Hint Sheet's get text
        'de': 'Du hast das {name} erhalten! Benutze es, wenn du nicht weiterweißt, um zu erfahren, wo dein nächster '
              'Schatz liegt.',
        'fr': 'Vous avez obtenu la {name} ! Utilisez-la quand vous êtes bloqué pour savoir où se trouve votre '
              'prochain trésor.',
        'fr_us': 'Tu as obtenu la {name}! Utilise-la quand tu es bloqué pour savoir où se trouve ton prochain trésor!',
        'es': '¡Has conseguido la {name}! Úsala cuando no sepas qué hacer para saber dónde está tu próximo tesoro.',
        'es_us': '¡Obtuviste la {name}! Úsala cuando no sepas qué hacer para saber dónde está tu próximo tesoro.',
        'it': 'Hai ottenuto il {name}! Usalo quando sei bloccato per scoprire dove si trova il tuo prossimo tesoro.',
        'nl': 'Je hebt het {name}! Gebruik het als je vastzit om te zien waar je volgende schat ligt.',
        'ru': 'Вы получили {name}! Используйте его, если застряли, чтобы узнать, где лежит следующее сокровище.',
        'ja': '{name}を 手に入れた！ 困った時に 使うと 次の宝の ありかが わかるぞ！',
        'ko': '{name}를 손에 넣었다! 막혔을 때 사용하면 다음 보물이 있는 곳을 알 수 있다!',
        'zh_cn': '得到了{name}！卡关时使用，就能知道下一件宝物在哪里！',
        'zh_tw': '得到了{name}！卡關時使用，就能知道下一件寶物在哪裡！',
    },
    'hint-name': {'de': 'Hinweisblatt', 'fr': "Feuille d'indices", 'fr_us': "Feuille d'indices", 'es': 'hoja de pistas',
                  'es_us': 'hoja de pistas', 'it': 'foglio degli indizi', 'nl': 'hintblad', 'ru': 'лист подсказок',
                  'ja': 'ヒントシート', 'ko': '힌트 시트', 'zh_cn': '提示纸', 'zh_tw': '提示紙'},
    'hint-done': {
        'de': 'Es gibt nichts mehr zu finden. Wecke den Windfisch!',
        'fr': "Il n'y a plus rien à trouver. Allez réveiller le Poisson-Rêve !",
        'fr_us': "Il n'y a plus rien à trouver. Va réveiller le Poisson-Rêve!",
        'es': 'No queda nada por encontrar. ¡Ve a despertar al Pez Viento!',
        'es_us': 'No queda nada por encontrar. ¡Ve a despertar al Pez Viento!',
        'it': "Non c'è più niente da trovare. Vai a svegliare il Pesce Vento!",
        'nl': 'Er valt niets meer te vinden. Ga de Windvis wekken!',
        'ru': 'Больше нечего искать. Разбудите Рыбу-ветер!',
        'ja': 'もう 探す物は ないぞ。 かぜのさかなを 起こしに 行こう！',
        'ko': '더 이상 찾을 것은 없다. 바람의 물고기를 깨우러 가자!',
        'zh_cn': '已经没有要找的东西了。去唤醒风之鱼吧！',
        'zh_tw': '已經沒有要找的東西了。去喚醒風之魚吧！',
    },
    # a hint: '<item>: <where>' (the item red); where = the place, its detail in brackets
    'colon': {'ja': '：', 'zh_cn': '：', 'zh_tw': '：'},
}
# hint details (where an item lies inside a place)
DETAIL = {
    'pot': {'de': 'in einem Topf', 'fr': 'dans une jarre', 'es': 'en una vasija', 'it': 'in un vaso', 'nl': 'in een pot',
            'ru': 'в горшке', 'ja': 'ツボの中', 'ko': '항아리 안', 'zh_cn': '罐子里', 'zh_tw': '罐子裡'},
    'bush': {'de': 'unter einem Busch', 'fr': 'sous un buisson', 'es': 'bajo un arbusto', 'it': 'sotto un cespuglio',
             'nl': 'onder een struik', 'ru': 'под кустом', 'ja': '草むらの下', 'ko': '풀숲 아래', 'zh_cn': '草丛下',
             'zh_tw': '草叢下'},
    'buried': {'de': 'vergraben', 'fr': 'sous terre', 'es': 'bajo tierra', 'it': 'sottoterra', 'nl': 'begraven',
               'ru': 'в земле', 'ja': '地面の中', 'ko': '땅속', 'zh_cn': '埋在地下', 'zh_tw': '埋在地下'},
    'tree': {'de': 'in einem Baum, mit Sprint dagegenrennen', 'fr': 'dans un arbre, foncez dedans',
             'fr_us': 'dans un arbre, fonce dedans', 'es': 'en un árbol, embístelo corriendo',
             'it': 'in un albero, colpiscilo di corsa', 'nl': 'in een boom, ren ertegenaan',
             'ru': 'в дереве, врежьтесь в него с разбега', 'ja': '木の中 ダッシュで 体当たり',
             'ko': '나무 안, 대시로 부딪치기', 'zh_cn': '树里，冲刺撞击', 'zh_tw': '樹裡，衝刺撞擊'},
    'underwater': {'de': 'unter Wasser', 'fr': "sous l'eau", 'es': 'bajo el agua', 'it': "sott'acqua",
                   'nl': 'onder water', 'ru': 'под водой', 'ja': '水の中', 'ko': '물속', 'zh_cn': '水底', 'zh_tw': '水底'},
    'boss': {'de': 'beim Endboss', 'fr': 'avec le boss', 'es': 'con el jefe', 'it': 'con il guardiano',
             'nl': 'bij de eindbaas', 'ru': 'у босса', 'ja': 'ボスの所', 'ko': '보스가 있는 곳', 'zh_cn': '首领处',
             'zh_tw': '首領處'},
    'end': {'de': 'ganz am Ende', 'fr': 'tout au fond', 'es': 'al final', 'it': 'alla fine', 'nl': 'helemaal aan het eind',
            'ru': 'в самом конце', 'ja': 'いちばん奥', 'ko': '가장 안쪽', 'zh_cn': '最深处', 'zh_tw': '最深處'},
    'race': {'de': 'Preis eines Rennens', 'fr': "prix d'une course", 'es': 'premio de una carrera',
             'it': 'premio di una gara', 'nl': 'prijs van een race', 'ru': 'приз за гонку', 'ja': 'レースの 賞品',
             'ko': '레이스 상품', 'zh_cn': '比赛奖品', 'zh_tw': '比賽獎品'},
    'shells': {'de': 'für {n} Muscheln', 'fr': 'pour {n} coquillages', 'es': 'por {n} caracolas',
               'it': 'per {n} conchiglie', 'nl': 'voor {n} schelpen', 'ru': 'за {n} ракушек', 'ja': '貝がら {n}個',
               'ko': '조개껍데기 {n}개', 'zh_cn': '{n}个贝壳', 'zh_tw': '{n}個貝殼'},
    'entrance': {'de': 'Eingang', 'fr': 'entrée', 'es': 'entrada', 'it': 'ingresso', 'nl': 'ingang', 'ru': 'вход',
                 'ja': '入口', 'ko': '입구', 'zh_cn': '入口', 'zh_tw': '入口'},
    'somewhere': {'de': 'irgendwo auf der Insel', 'fr': "quelque part sur l'île", 'es': 'en algún lugar de la isla',
                  'it': "da qualche parte sull'isola", 'nl': 'ergens op het eiland', 'ru': 'где-то на острове',
                  'ja': '島の どこか', 'ko': '섬 어딘가', 'zh_cn': '岛上某处', 'zh_tw': '島上某處'},
}
# items the game has no name for
ITEM_OWN = {
    'triforce-piece': {'de': 'Triforce-Fragment', 'fr': 'Fragment de Triforce', 'es': 'Fragmento de la Trifuerza',
                       'it': 'Frammento della Triforza', 'nl': 'Triforce-fragment', 'ru': 'Осколок Трифорса',
                       'ja': 'トライフォースのかけら', 'ko': '트라이포스 조각', 'zh_cn': '三角力量碎片', 'zh_tw': '三角神力碎片'},
    'bomb-capacity': {'de': 'Größere Bombentasche', 'fr': 'Sac de bombes plus grand', 'es': 'Bolsa de bombas mayor',
                      'it': 'Borsa per bombe più grande', 'nl': 'Grotere bommenzak', 'ru': 'Большая сумка для бомб',
                      'ja': 'ボム袋の 拡張', 'ko': '폭탄 가방 확장', 'zh_cn': '炸弹袋扩容', 'zh_tw': '炸彈袋擴容'},
    'arrow-capacity': {'de': 'Größerer Köcher', 'fr': 'Carquois plus grand', 'es': 'Carcaj mayor',
                       'it': 'Faretra più grande', 'nl': 'Grotere pijlkoker', 'ru': 'Большой колчан', 'ja': '矢筒の 拡張',
                       'ko': '화살통 확장', 'zh_cn': '箭袋扩容', 'zh_tw': '箭袋擴容'},
    'powder-capacity': {'de': 'Größerer Pulverbeutel', 'fr': 'Sac de poudre plus grand', 'es': 'Bolsa de polvos mayor',
                        'it': 'Sacchetto di polvere più grande', 'nl': 'Groter poederzakje',
                        'ru': 'Большой мешочек для порошка', 'ja': '粉袋の 拡張', 'ko': '가루 주머니 확장',
                        'zh_cn': '魔粉袋扩容', 'zh_tw': '魔粉袋擴容'},
    'stalfos-note': {'de': 'Notiz von Meister Stalfos', 'fr': 'Note de Maître Stalfos', 'es': 'Nota del Maestro Stalfos',
                     'it': 'Biglietto di Mastro Stalfos', 'nl': 'Briefje van Meester Stalfos',
                     'ru': 'Записка Мастера Сталфоса', 'ja': 'マスタースタルフォスの メモ', 'ko': '마스터 스탈포스의 메모',
                     'zh_cn': '骷髅大师的字条', 'zh_tw': '骷髏大師的字條'},
    'rooster': {'de': 'Fliegender Hahn', 'fr': 'Coq volant', 'es': 'Gallo volador', 'it': 'Gallo volante',
                'nl': 'Vliegende haan', 'ru': 'Летающий петух', 'ja': '空飛ぶ ニワトリ', 'ko': '하늘을 나는 닭',
                'zh_cn': '飞翔的公鸡', 'zh_tw': '飛翔的公雞'},
    'bow-wow': {'ja': 'ワンワン', 'ko': '멍멍', 'zh_cn': '汪汪', 'zh_tw': '汪汪'},
    'ghost': {'de': 'Geist', 'fr': 'Fantôme', 'es': 'Fantasma', 'it': 'Fantasma', 'nl': 'Geest', 'ru': 'Призрак',
              'ja': 'オバケ', 'ko': '유령', 'zh_cn': '幽灵', 'zh_tw': '幽靈'},
    'marin-companion': {'ja': 'マリン', 'ko': '마린', 'zh_cn': '玛琳', 'zh_tw': '瑪琳'},
    'soul': {'de': 'Seele', 'fr': 'Âme', 'es': 'Alma', 'it': 'Anima', 'nl': 'Ziel', 'ru': 'Душа', 'ja': '魂', 'ko': '혼',
             'zh_cn': '灵魂', 'zh_tw': '靈魂'},
}


def fill(template, **parts):
    """'Du hast die {name} erhalten!' -> segments; parts: name -> (text or game bytes, red)."""
    out = []
    for piece in re.split(r'(\{\w+\})', template):
        if re.fullmatch(r'\{\w+\}', piece):
            out.append(parts[piece[1:-1]])
        elif piece:
            out.append((piece, False))
    return out


def tr(table, lang):
    """table[lang], American French / Spanish falling back to the European text, anything else to None."""
    return table.get(lang) or table.get(lang[:2]) or None
