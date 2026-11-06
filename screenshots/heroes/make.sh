#!/bin/sh
# Makes the READMEs' hero screenshots from headless runs, so they can be made again as the engine changes.
#
#   screenshots/heroes/make.sh [hero...]     # from the repository root; every hero when none is named
#
# Needs the set variant built (./ngen-cli build), its asset server running in the repository root (./ngen-cli asset-server &),
# the streaming test world under assets/streamtest-world/, ImageMagick (magick) and ffmpeg. Each hero runs ngen-view (or
# ngen-introspect) with its script in this folder, writes its frames under _out/heroes/<hero>/, and composes them into
# screenshots/<library>_hero.png (view: a GIF).

set -eu

world=assets/streamtest-world/world/root.usda
out=_out/heroes
background='#f8fafc'
ink='#1f2733'

view() {
    SDL_VIDEODRIVER=offscreen ./ngen-cli view "$@" --fail-on-validation
}

# A thumbnail of $1 with the label $2 under it, $3 wide, written to $4.
labelled() {
    magick "$1" -resize "$3x" -background "$background" -fill "$ink" -font DejaVu-Sans -pointsize 15 \
        label:"$2" -gravity center -append "$4"
}

renderer() {
    rm -rf "$out/renderer" && mkdir -p "$out/renderer"
    view "$world" --camera=/cameras/castle --script=screenshots/heroes/renderer.txt > "$out/renderer.log" 2>&1
    cd "$out/renderer"
    magick -size 5x1 xc:"$background" gap.png
    labelled albedo.png "albedo" 316 t1.png
    labelled normals.png "normals" 316 t2.png
    labelled depth.png "depth" 316 t3.png
    labelled cascades.png "shadow cascades" 316 t4.png
    labelled instance.png "instance ids" 316 t5.png
    magick t1.png gap.png t2.png gap.png t3.png gap.png t4.png gap.png t5.png -background "$background" +append strip.png
    magick lit.png -resize 1600x -size 1600x5 xc:"$background" strip.png -append -strip ../../../screenshots/renderer_hero.png
    cd - > /dev/null
}

heroes=${*:-renderer}
for hero in $heroes; do
    echo "hero: $hero"
    "$hero"
done
