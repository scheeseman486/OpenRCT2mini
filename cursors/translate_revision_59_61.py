#!/usr/bin/env python3
# OPENRCT2MINI revision 61 helper: replace the upstream STR_6785..6791
# (Gamepad / Deadzone / Sensitivity) translations with our re-purposed
# Virtual mouse / Acceleration / Speed wording, and append the new
# STR_7030..7035 cursor-theme strings.
#
# Only touches languages that already had translations for STR_6785..6791.
# Languages without those entries fall back through OpenRCT2's
# parent-locale chain (and ultimately to en-GB), so leaving them empty
# is consistent with their current behaviour.
#
# Run from the repo root:
#   python3 cursors/translate_revision_59_61.py

import os
import re
import sys

LANG_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "language")

TRANSLATIONS = {
    "ca-ES": {  # Catalan
        6785: "Ratolí virtual",
        6786: "Acceleració:",
        6787: "Velocitat a què el D-pad assoleix la velocitat màxima del ratolí virtual",
        6788: "Velocitat:",
        6789: "Multiplicador de velocitat del ratolí virtual",
        6790: "Acceleració: {COMMA32}%",
        6791: "Velocitat: {COMMA32}%",
        7030: "Cursors",
        7031: "Estil de cursor:",
        7032: "Tria com es dibuixa el cursor a la pantalla",
        7033: "Per defecte",
        7034: "Alt contrast",
        7035: "Clàssic",
    },
    "cs-CZ": {  # Czech
        6785: "Virtuální myš",
        6786: "Akcelerace:",
        6787: "Jak rychle D-pad dosáhne maximální rychlosti virtuální myši",
        6788: "Rychlost:",
        6789: "Násobitel rychlosti virtuální myši",
        6790: "Akcelerace: {COMMA32}%",
        6791: "Rychlost: {COMMA32}%",
        7030: "Kurzory",
        7031: "Styl kurzoru:",
        7032: "Vyberte, jak se vykresluje kurzor na obrazovce",
        7033: "Výchozí",
        7034: "Vysoký kontrast",
        7035: "Klasický",
    },
    "da-DK": {  # Danish
        6785: "Virtuel mus",
        6786: "Acceleration:",
        6787: "Hvor hurtigt D-pad'en når den virtuelle mus' maksimumhastighed",
        6788: "Hastighed:",
        6789: "Multiplikator for virtuel mus-hastighed",
        6790: "Acceleration: {COMMA32}%",
        6791: "Hastighed: {COMMA32}%",
        7030: "Markører",
        7031: "Markørstil:",
        7032: "Vælg hvordan skærmens markør tegnes",
        7033: "Standard",
        7034: "Høj kontrast",
        7035: "Klassisk",
    },
    "de-DE": {  # German
        6785: "Virtuelle Maus",
        6786: "Beschleunigung:",
        6787: "Wie schnell das D-Pad die maximale Geschwindigkeit der virtuellen Maus erreicht",
        6788: "Geschwindigkeit:",
        6789: "Multiplikator der virtuellen Mausgeschwindigkeit",
        6790: "Beschleunigung: {COMMA32}%",
        6791: "Geschwindigkeit: {COMMA32}%",
        7030: "Mauszeiger",
        7031: "Zeigerstil:",
        7032: "Wählt, wie der Bildschirmzeiger gezeichnet wird",
        7033: "Standard",
        7034: "Hoher Kontrast",
        7035: "Klassisch",
    },
    "eo-ZZ": {  # Esperanto
        6785: "Virtuala muso",
        6786: "Akcelo:",
        6787: "Kiel rapide la D-pad atingas la maksimuman rapidon de la virtuala muso",
        6788: "Rapido:",
        6789: "Obligilo de la rapido de la virtuala muso",
        6790: "Akcelo: {COMMA32}%",
        6791: "Rapido: {COMMA32}%",
        7030: "Kursoroj",
        7031: "Stilo de kursoro:",
        7032: "Elektu kiel la ekrana kursoro estas desegnita",
        7033: "Defaŭlta",
        7034: "Alta kontrasto",
        7035: "Klasika",
    },
    "es-ES": {  # Spanish
        6785: "Ratón virtual",
        6786: "Aceleración:",
        6787: "Qué tan rápido el D-pad alcanza la velocidad máxima del ratón virtual",
        6788: "Velocidad:",
        6789: "Multiplicador de velocidad del ratón virtual",
        6790: "Aceleración: {COMMA32}%",
        6791: "Velocidad: {COMMA32}%",
        7030: "Cursores",
        7031: "Estilo del cursor:",
        7032: "Elige cómo se dibuja el cursor en la pantalla",
        7033: "Predeterminado",
        7034: "Alto contraste",
        7035: "Clásico",
    },
    "fr-FR": {  # French
        6785: "Souris virtuelle",
        6786: "Accélération :",
        6787: "Vitesse à laquelle le D-pad atteint la vitesse maximale de la souris virtuelle",
        6788: "Vitesse :",
        6789: "Multiplicateur de vitesse de la souris virtuelle",
        6790: "Accélération : {COMMA32}%",
        6791: "Vitesse : {COMMA32}%",
        7030: "Curseurs",
        7031: "Style du curseur :",
        7032: "Choisissez le rendu du curseur à l'écran",
        7033: "Par défaut",
        7034: "Contraste élevé",
        7035: "Classique",
    },
    "gl-ES": {  # Galician
        6785: "Rato virtual",
        6786: "Aceleración:",
        6787: "Que rápido o D-pad acada a velocidade máxima do rato virtual",
        6788: "Velocidade:",
        6789: "Multiplicador da velocidade do rato virtual",
        6790: "Aceleración: {COMMA32}%",
        6791: "Velocidade: {COMMA32}%",
        7030: "Cursores",
        7031: "Estilo do cursor:",
        7032: "Elixe como se debuxa o cursor na pantalla",
        7033: "Predeterminado",
        7034: "Alto contraste",
        7035: "Clásico",
    },
    "hu-HU": {  # Hungarian
        6785: "Virtuális egér",
        6786: "Gyorsulás:",
        6787: "Milyen gyorsan éri el a D-pad a virtuális egér maximális sebességét",
        6788: "Sebesség:",
        6789: "Virtuális egér sebességszorzója",
        6790: "Gyorsulás: {COMMA32}%",
        6791: "Sebesség: {COMMA32}%",
        7030: "Kurzorok",
        7031: "Kurzorstílus:",
        7032: "Válassza ki, hogyan jelenjen meg a képernyőkurzor",
        7033: "Alapértelmezett",
        7034: "Magas kontraszt",
        7035: "Klasszikus",
    },
    "nl-NL": {  # Dutch
        6785: "Virtuele muis",
        6786: "Versnelling:",
        6787: "Hoe snel het D-pad de maximumsnelheid van de virtuele muis bereikt",
        6788: "Snelheid:",
        6789: "Snelheidsvermenigvuldiger van de virtuele muis",
        6790: "Versnelling: {COMMA32}%",
        6791: "Snelheid: {COMMA32}%",
        7030: "Cursors",
        7031: "Cursorstijl:",
        7032: "Kies hoe de cursor op het scherm wordt getekend",
        7033: "Standaard",
        7034: "Hoog contrast",
        7035: "Klassiek",
    },
    "pl-PL": {  # Polish
        6785: "Wirtualna mysz",
        6786: "Przyspieszenie:",
        6787: "Jak szybko D-pad osiąga maksymalną prędkość wirtualnej myszy",
        6788: "Prędkość:",
        6789: "Mnożnik prędkości wirtualnej myszy",
        6790: "Przyspieszenie: {COMMA32}%",
        6791: "Prędkość: {COMMA32}%",
        7030: "Kursory",
        7031: "Styl kursora:",
        7032: "Wybierz, jak rysowany jest kursor na ekranie",
        7033: "Domyślny",
        7034: "Wysoki kontrast",
        7035: "Klasyczny",
    },
    "pt-BR": {  # Portuguese (Brazilian)
        6785: "Mouse virtual",
        6786: "Aceleração:",
        6787: "Quão rápido o D-pad atinge a velocidade máxima do mouse virtual",
        6788: "Velocidade:",
        6789: "Multiplicador de velocidade do mouse virtual",
        6790: "Aceleração: {COMMA32}%",
        6791: "Velocidade: {COMMA32}%",
        7030: "Cursores",
        7031: "Estilo do cursor:",
        7032: "Escolha como o cursor é desenhado na tela",
        7033: "Padrão",
        7034: "Alto contraste",
        7035: "Clássico",
    },
    "ru-RU": {  # Russian
        6785: "Виртуальная мышь",
        6786: "Ускорение:",
        6787: "Насколько быстро D-pad достигает максимальной скорости виртуальной мыши",
        6788: "Скорость:",
        6789: "Множитель скорости виртуальной мыши",
        6790: "Ускорение: {COMMA32}%",
        6791: "Скорость: {COMMA32}%",
        7030: "Курсоры",
        7031: "Стиль курсора:",
        7032: "Выберите, как рисуется курсор на экране",
        7033: "По умолчанию",
        7034: "Высокий контраст",
        7035: "Классический",
    },
    "sv-SE": {  # Swedish
        6785: "Virtuell mus",
        6786: "Acceleration:",
        6787: "Hur snabbt D-paden når den virtuella musens maxhastighet",
        6788: "Hastighet:",
        6789: "Hastighetsmultiplikator för den virtuella musen",
        6790: "Acceleration: {COMMA32}%",
        6791: "Hastighet: {COMMA32}%",
        7030: "Markörer",
        7031: "Markörstil:",
        7032: "Välj hur skärmens markör ritas",
        7033: "Standard",
        7034: "Hög kontrast",
        7035: "Klassisk",
    },
}


def update_lang(lang_code: str, mapping: dict) -> int:
    path = os.path.join(LANG_DIR, f"{lang_code}.txt")
    if not os.path.isfile(path):
        print(f"  skip {lang_code}: file not found", file=sys.stderr)
        return 0

    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    # Track newline used by the file (assumes LF — checked by spot inspection).
    nl = "\n"

    replaced = 0
    appended = 0

    for sid, value in mapping.items():
        # Match: STR_NNNN<spaces>:<...>\n
        pattern = re.compile(rf"^(STR_{sid:04d}\s+:).*$", re.MULTILINE)
        new_line = f"STR_{sid:04d}    :{value}"
        if pattern.search(text):
            text, n = pattern.subn(new_line, text, count=1)
            replaced += n
        else:
            # Append at EOF — keep the file ending with a newline.
            if not text.endswith(nl):
                text += nl
            text += new_line + nl
            appended += 1

    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"  {lang_code}: replaced={replaced}, appended={appended}")
    return replaced + appended


def main():
    total = 0
    for code in sorted(TRANSLATIONS):
        total += update_lang(code, TRANSLATIONS[code])
    print(f"Done — {total} entries updated across {len(TRANSLATIONS)} languages.")


if __name__ == "__main__":
    main()
