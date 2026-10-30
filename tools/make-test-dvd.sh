#!/usr/bin/env bash
# Build a small synthetic NTSC DVD with menus, for exercising disc navigation.
#
#   VMGM menu   still (pause=inf), NO audio, 3 buttons:
#                 [Play Title 1] [Play Title 2] [Motion Menu]
#   VTS menu    6 s looping motion menu WITH audio, 2 buttons:
#                 [Submenu] [Play Title 1]
#   VTS menu 2  still submenu without a "go up", 1 button: [Main Menu]
#   Title 1     20 s, chapters every 5 s, timecode burned in -> back to VMGM menu
#   Title 2     10 s colour bars, forced subtitle 1-8 s (orange box,
#               cyan inside: checks the disc's palette)       -> back to VMGM menu
#
# Usage: tools/make-test-dvd.sh [outdir]   (default: build/fixtures/discnav-test)
# Produces <outdir>/DISC/VIDEO_TS and <outdir>/discnav-test.iso
set -euo pipefail

out=${1:-build/fixtures/discnav-test}
font=$(fc-match -f '%{file}' 'DejaVu Sans:bold')
work=$out/work
rm -rf "$out"
mkdir -p "$work"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
txt() { # text, x, y, size
    printf "drawtext=fontfile=%s:text='%s':x=%s:y=%s:fontsize=%s:fontcolor=white" \
        "$font" "$1" "$2" "$3" "$4"
}

# Button geometry (720x480 storage coords) shared by graphics and spumux.
vmgm_btns=("200 150 520 200" "200 230 520 280" "200 310 520 360")
vts_btns=("100 360 330 410" "390 360 620 410")
sub_btns=("240 310 480 360")

# Outline highlight/select overlays for spumux (<=4 colours, transparent bg).
overlay() { # out.png colour geometry...
    local png=$1 colour=$2 vf=""; shift 2
    for g in "$@"; do
        read -r x0 y0 x1 y1 <<<"$g"
        vf+="drawbox=x=$x0:y=$y0:w=$((x1 - x0)):h=$((y1 - y0)):color=$colour:t=6,"
    done
    # drawbox keeps a transparent canvas's alpha at 0, so draw on opaque
    # black and key the black out afterwards
    ff -f lavfi -i "color=c=black:s=720x480" \
        -vf "${vf%,},format=rgba,colorkey=black:0.01:0" -frames:v 1 "$png"
}

spumux_xml() { # xml hl.png sel.png "name geometry up down"...
    local xml=$1 hl=$2 sel=$3; shift 3
    {
        echo '<subpictures format="NTSC"><stream>'
        echo "<spu start=\"00:00:00.00\" force=\"yes\" highlight=\"$hl\" select=\"$sel\">"
        for b in "$@"; do
            read -r name x0 y0 x1 y1 up down left right <<<"$b"
            echo "  <button name=\"$name\" x0=\"$x0\" y0=\"$y0\" x1=\"$x1\" y1=\"$y1\" up=\"$up\" down=\"$down\" left=\"$left\" right=\"$right\"/>"
        done
        echo '</spu></stream></subpictures>'
    } >"$xml"
}

label_boxes() { # filter string drawing filled button boxes + labels
    local vf="" i=0; local -n geo=$1 labels=$2
    for g in "${geo[@]}"; do
        read -r x0 y0 x1 y1 <<<"$g"
        vf+="drawbox=x=$x0:y=$y0:w=$((x1 - x0)):h=$((y1 - y0)):color=0x334477:t=fill,"
        vf+="$(txt "${labels[$i]}" "$((x0 + 20))" "$((y0 + 12))" 26),"
        i=$((i + 1))
    done
    echo "${vf%,}"
}

echo ">> VMGM still menu (no audio)"
vmgm_labels=("Play Title 1" "Play Title 2" "Motion Menu")
ff -f lavfi -i "color=c=0x101830:s=720x480:r=30000/1001" -t 1 \
    -vf "$(txt 'DISCNAV TEST DISC' 160 60 40),$(label_boxes vmgm_btns vmgm_labels)" \
    -an -target ntsc-dvd -aspect 4:3 "$work/vmgm_bg.mpg"
overlay "$work/vmgm_hl.png" yellow "${vmgm_btns[@]}"
overlay "$work/vmgm_sel.png" red "${vmgm_btns[@]}"
spumux_xml "$work/vmgm.xml" "$work/vmgm_hl.png" "$work/vmgm_sel.png" \
    "b1 ${vmgm_btns[0]} b3 b2 b1 b1" \
    "b2 ${vmgm_btns[1]} b1 b3 b2 b2" \
    "b3 ${vmgm_btns[2]} b2 b1 b3 b3"
spumux -s0 "$work/vmgm.xml" <"$work/vmgm_bg.mpg" >"$work/vmgm_menu.mpg" 2>"$work/spumux-vmgm.log"

echo ">> VTS motion menu (with audio)"
vts_labels=("Submenu" "Play Title 1")
ff -f lavfi -i "testsrc2=s=720x480:r=30000/1001" -f lavfi -i "sine=f=330:sample_rate=48000" -t 6 \
    -vf "drawbox=x=0:y=0:w=720:h=120:color=black@0.7:t=fill,$(txt 'MOTION MENU (loops)' 150 40 40),$(label_boxes vts_btns vts_labels)" \
    -target ntsc-dvd -aspect 4:3 "$work/vts_bg.mpg"
overlay "$work/vts_hl.png" yellow "${vts_btns[@]}"
overlay "$work/vts_sel.png" red "${vts_btns[@]}"
spumux_xml "$work/vts.xml" "$work/vts_hl.png" "$work/vts_sel.png" \
    "b1 ${vts_btns[0]} b1 b1 b2 b2" \
    "b2 ${vts_btns[1]} b2 b2 b1 b1"
spumux -s0 "$work/vts.xml" <"$work/vts_bg.mpg" >"$work/vts_menu.mpg" 2>"$work/spumux-vts.log"

echo ">> VTS still submenu (no audio)"
sub_labels=("Main Menu")
ff -f lavfi -i "color=c=0x203010:s=720x480:r=30000/1001" -t 1 \
    -vf "$(txt 'SUBMENU' 260 60 40),$(label_boxes sub_btns sub_labels)" \
    -an -target ntsc-dvd -aspect 4:3 "$work/sub_bg.mpg"
overlay "$work/sub_hl.png" yellow "${sub_btns[@]}"
overlay "$work/sub_sel.png" red "${sub_btns[@]}"
spumux_xml "$work/sub.xml" "$work/sub_hl.png" "$work/sub_sel.png" "b1 ${sub_btns[0]} b1 b1 b1 b1"
spumux -s0 "$work/sub.xml" <"$work/sub_bg.mpg" >"$work/sub_menu.mpg" 2>"$work/spumux-sub.log"

echo ">> titles"
ff -f lavfi -i "testsrc2=s=720x480:r=30000/1001" -f lavfi -i "sine=f=440:sample_rate=48000" -t 20 \
    -vf "$(txt 'TITLE 1' 40 40 48),$(txt '%{pts\:hms}' 40 400 40)" \
    -target ntsc-dvd -aspect 4:3 "$work/title1.mpg"
ff -f lavfi -i "smptebars=s=720x480:r=30000/1001" -f lavfi -i "sine=f=880:sample_rate=48000" -t 10 \
    -vf "$(txt 'TITLE 2' 40 40 48),$(txt '%{pts\:hms}' 40 400 40)" \
    -target ntsc-dvd -aspect 4:3 "$work/title2.mpg"
# forced subtitle: drawn on opaque black, black keyed out (see overlay())
ff -f lavfi -i "color=c=black:s=720x480" \
    -vf "drawbox=x=260:y=200:w=200:h=80:color=0xff8000:t=fill,drawbox=x=300:y=220:w=120:h=40:color=0x00ffff:t=fill,format=rgba,colorkey=black:0.01:0" \
    -frames:v 1 "$work/title2_sub.png"
cat >"$work/title2_sub.xml" <<EOF
<subpictures format="NTSC"><stream>
<spu start="00:00:01.00" end="00:00:08.00" force="yes" image="$work/title2_sub.png"/>
</stream></subpictures>
EOF
spumux -s0 "$work/title2_sub.xml" <"$work/title2.mpg" >"$work/title2_subs.mpg" 2>"$work/spumux-title2.log"

echo ">> dvdauthor"
cat >"$work/dvd.xml" <<EOF
<dvdauthor dest="$out/DISC">
  <vmgm>
    <fpc>jump menu 1;</fpc>
    <menus>
      <video format="ntsc" aspect="4:3"/>
      <pgc entry="title">
        <button name="b1">jump title 1;</button>
        <button name="b2">jump title 2;</button>
        <button name="b3">jump titleset 1 menu;</button>
        <vob file="$work/vmgm_menu.mpg" pause="inf"/>
      </pgc>
    </menus>
  </vmgm>
  <titleset>
    <menus>
      <video format="ntsc" aspect="4:3"/>
      <pgc entry="root">
        <button name="b1">jump menu 2;</button>
        <button name="b2">jump title 1;</button>
        <vob file="$work/vts_menu.mpg"/>
        <post>jump cell 1;</post>
      </pgc>
      <pgc>
        <button name="b1">jump menu 1;</button>
        <vob file="$work/sub_menu.mpg" pause="inf"/>
      </pgc>
    </menus>
    <titles>
      <video format="ntsc" aspect="4:3"/>
      <audio lang="en"/>
      <subpicture lang="en"/>
      <pgc>
        <vob file="$work/title1.mpg" chapters="0,0:05,0:10,0:15"/>
        <post>call vmgm menu 1;</post>
      </pgc>
      <pgc>
        <vob file="$work/title2_subs.mpg"/>
        <post>call vmgm menu 1;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
EOF
VIDEO_FORMAT=NTSC dvdauthor -x "$work/dvd.xml" >"$work/dvdauthor.log" 2>&1 ||
    { cat "$work/dvdauthor.log"; exit 1; }

echo ">> iso"
mkisofs -quiet -dvd-video -V DISCNAV_TEST -o "$out/discnav-test.iso" "$out/DISC"

echo "done: $out/DISC/VIDEO_TS, $out/discnav-test.iso"
