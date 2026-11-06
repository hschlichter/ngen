#!/bin/sh
# Makes the READMEs' hero screenshots from headless runs, so they can be made again as the engine changes.
#
#   screenshots/heroes/make.sh [hero...]     # from the repository root; every hero when none is named
#
# Heroes: renderer, view, introspect, trace, config, tools, rhi. Needs the set variant built (./ngen-cli build) and the debug variant's
# RHI examples (./_out/ngen-build -p linux-vulkan -c debug examples), the set variant's asset server running in the repository root
# (./ngen-cli asset-server &), no other ngen-view running there, the streaming test world under assets/streamtest-world/, ImageMagick
# (magick) and ffmpeg. Each hero writes its frames under _out/heroes/<hero>/ and composes them into screenshots/<library>_hero.png (the
# view's is a GIF). The view scripts are the .txt files beside this one.

set -eu

world=assets/streamtest-world/world/root.usda
out=_out/heroes
shots=screenshots
background='#f8fafc'
ink='#1f2733'

view() {
    SDL_VIDEODRIVER=offscreen ./ngen-cli view "$@" --fail-on-validation
}

call() {
    ./ngen-cli introspect call view "$@"
}

# Starts ngen-view in the background with the given arguments, and waits until it answers RPC calls.
startView() {
    SDL_VIDEODRIVER=offscreen ./ngen-cli view "$@" > "$out/live.log" 2>&1 &
    until call rpc.version > /dev/null 2>&1; do
        sleep 0.5
    done
}

# A thumbnail of $1 with the label $2 under it, $3 wide, written to $4.
labelled() {
    magick "$1" -resize "$3x" -background "$background" -fill "$ink" -font DejaVu-Sans -pointsize 15 \
        label:"$2" -gravity center -append "$4"
}

fresh() {
    rm -rf "${out:?}/$1"
    mkdir -p "$out/$1"
}

# The lit castle, and under it what the passes made before it.
hero_renderer() {
    fresh renderer
    view "$world" --camera=/cameras/castle --script=$shots/heroes/renderer.txt > "$out/renderer.log" 2>&1
    dir=$out/renderer
    magick -size 5x1 xc:"$background" "$dir/gap.png"
    labelled "$dir/albedo.png" "albedo" 316 "$dir/t1.png"
    labelled "$dir/normals.png" "normals" 316 "$dir/t2.png"
    labelled "$dir/depth.png" "depth" 316 "$dir/t3.png"
    labelled "$dir/cascades.png" "shadow cascades" 316 "$dir/t4.png"
    labelled "$dir/instance.png" "instance ids" 316 "$dir/t5.png"
    magick "$dir/t1.png" "$dir/gap.png" "$dir/t2.png" "$dir/gap.png" "$dir/t3.png" "$dir/gap.png" "$dir/t4.png" "$dir/gap.png" "$dir/t5.png" \
        -background "$background" +append "$dir/strip.png"
    magick "$dir/lit.png" -resize 1600x -size 1600x5 xc:"$background" "$dir/strip.png" -append -strip -depth 8 $shots/renderer_hero.png
}

# A flight from flight_20 to flight_29 in the LOD level view: each LOD root's drawn level colours what it draws as the camera moves.
# A flight holds a script's clock, so the frames are taken over RPC while it flies.
hero_view() {
    fresh view
    startView "$world" --camera=/cameras/flight_20 --view=lodlevel
    call view.waitResident '{}' > /dev/null
    call view.camera.fly '{"prefix": "/cameras/flight_2", "speed": 60}' > "$out/view/fly.json" &
    flight=$!
    i=0
    while kill -0 $flight 2> /dev/null; do
        call view.screenshot "{\"path\": \"$out/view/f$(printf %04d $i).png\"}" > /dev/null
        i=$((i + 1))
    done
    call view.quit > /dev/null
    wait
    # Every other frame at 6 a second, 720 wide, 96 colours without dithering: about 3 MB.
    ffmpeg -loglevel error -y -framerate 10 -i "$out/view/f%04d.png" \
        -vf "select='not(mod(n\,2))',setpts=N/6/TB,scale=720:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
        -r 6 -loop 0 $shots/view_hero.gif
}

# The introspect window's Records tab on the view's assets record, the castle loaded.
hero_introspect() {
    fresh introspect
    startView "$world" --camera=/cameras/castle
    call view.waitResident '{}' > /dev/null
    SDL_VIDEODRIVER=offscreen ./ngen-cli introspect --select=view/assets --frames=900 --screenshot="$out/introspect/shot.png" > "$out/introspect.log" 2>&1
    call view.quit > /dev/null
    wait
    magick "$out/introspect/shot.png" -strip -depth 8 $shots/introspect_hero.png
}

# The introspect window's Trace tab across a short run of the layered scene: the view's and the asset server's events merged by time.
hero_trace() {
    fresh trace
    SDL_VIDEODRIVER=offscreen ./ngen-cli introspect --trace --frames=9000 --screenshot="$out/trace/shot.png" > "$out/trace.log" 2>&1 &
    window=$!
    sleep 1
    view assets/layered/root.usda --frames=400 > /dev/null 2>&1
    wait $window
    magick "$out/trace/shot.png" -strip -depth 8 $shots/trace_hero.png
}

# The Configuration window over the castle, cropped to it.
hero_config() {
    fresh config
    view "$world" --camera=/cameras/castle --show-ui --script=$shots/heroes/config.txt > "$out/config.log" 2>&1
    magick "$out/config/shot.png" -crop 1100x640+0+0 +repage -strip -depth 8 $shots/config_hero.png
}

# The debug camera above the castle with the streaming roots' bounds and the Streaming window.
hero_tools() {
    fresh tools
    view "$world" --camera=/cameras/castle --show-ui --script=$shots/heroes/tools.txt > "$out/tools.log" 2>&1
    magick "$out/tools/shot.png" -resize 1920x -strip -depth 8 $shots/tools_hero.png
}

# Every RHI example's last frame, labelled.
hero_rhi() {
    fresh rhi
    for program in _out/linux-vulkan/debug/ngen-example-*; do
        name=${program##*/ngen-example-}
        SDL_VIDEODRIVER=offscreen "$program" --frames=10 --check --screenshot="$out/rhi/$name.png" > /dev/null 2>&1
    done
    magick montage -font DejaVu-Sans -pointsize 18 -label '%t' "$out"/rhi/*.png -tile 5x -geometry 384x216+6+6 -background "$background" \
        -strip -depth 8 $shots/rhi_hero.png
}

heroes=${*:-renderer view introspect trace config tools rhi}
for hero in $heroes; do
    echo "hero: $hero"
    "hero_$hero"
done
