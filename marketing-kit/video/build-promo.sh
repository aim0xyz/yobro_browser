#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT/marketing-kit/video"
WORK_DIR="$OUT_DIR/.render"
MAGICK="${MAGICK:-magick}"
FFMPEG="${FFMPEG:-ffmpeg}"

SANS="/System/Library/Fonts/SFNS.ttf"
SERIF="/System/Library/Fonts/Supplemental/Georgia.ttf"
MARK="$ROOT/Resources/YOBRO-logo-master.png"
HERO="$ROOT/marketing-kit/assets/yobro-social-hero.png"
PRODUCT="$ROOT/marketing-kit/assets/yobro-product-spaces-dark.png"
AGENT_SPLIT="$ROOT/marketing-kit/assets/yobro-agent-split-mock.png"
FINAL="$OUT_DIR/yobro-promo-mockdata-1080p.mp4"
POSTER="$OUT_DIR/yobro-promo-poster.png"

mkdir -p "$WORK_DIR"

make_background() {
  local output="$1"
  local top="$2"
  local bottom="$3"
  "$MAGICK" -size 1920x1080 "gradient:${top}-${bottom}" +profile '*' "$output"
}

make_background "$WORK_DIR/dark.png" '#1f3028' '#0d1512'
make_background "$WORK_DIR/cream.png" '#fbf7ef' '#eee8dc'

round_image() {
  local input="$1"
  local width="$2"
  local height="$3"
  local radius="$4"
  local output="$5"
  "$MAGICK" "$input" -resize "${width}x${height}^" -gravity center -crop "${width}x${height}+0+0" +repage \
    \( -size "${width}x${height}" xc:none -fill white -draw "roundrectangle 0,0 $((width - 1)),$((height - 1)) ${radius},${radius}" \) \
    -alpha off -compose CopyOpacity -composite +profile '*' "$output"
}

round_image "$HERO" 1680 930 46 "$WORK_DIR/hero-rounded.png"
round_image "$PRODUCT" 1680 900 46 "$WORK_DIR/product-rounded.png"
round_image "$AGENT_SPLIT" 1600 850 46 "$WORK_DIR/agent-split-rounded.png"

# Opening card.
"$MAGICK" "$WORK_DIR/dark.png" \
  \( "$MARK" -resize 92x92 \) -geometry +144+150 -composite \
  -font "$SANS" -fill '#fffaf0' -pointsize 74 -draw "text 264,221 'YOBRO'" \
  -font "$SERIF" -fill '#fffaf0' -pointsize 92 -draw "text 144,440 'The browser that'" \
  -fill '#b7cfb3' -draw "text 144,548 'has your back.'" \
  -font "$SANS" -fill '#aab7af' -pointsize 34 -draw "text 148,650 'Native for macOS. Built for focused work.'" \
  +profile '*' "$WORK_DIR/scene-1.png"

# Hero: the source artwork itself contains only the fictional Northstar workspace.
"$MAGICK" "$WORK_DIR/cream.png" \
  -font "$SERIF" -fill '#20312a' -pointsize 78 -draw "text 120,145 'Less tab chaos.'" \
  -font "$SANS" -fill '#6d7872' -pointsize 34 -draw "text 124,205 'More clarity. More focus.'" \
  \( "$WORK_DIR/hero-rounded.png" \
     \( +clone -background black -shadow 32x18+0+18 \) +swap -background none -layers merge +repage \) \
  -gravity south -geometry +0+45 -composite \
  +profile '*' "$WORK_DIR/scene-2.png"

# Product overview.
"$MAGICK" "$WORK_DIR/dark.png" \
  \( "$WORK_DIR/product-rounded.png" \
     \( +clone -background black -shadow 34x16+0+18 \) +swap -background none -layers merge +repage \) \
  -gravity south -geometry +0+34 -composite \
  +profile '*' "$WORK_DIR/scene-3.png"

# Side-by-side agent feature captured from a fresh, isolated mock profile.
"$MAGICK" "$WORK_DIR/cream.png" \
  -font "$SERIF" -fill '#20312a' -pointsize 56 -draw "text 160,116 'You browse. Your agent works right beside you.'" \
  \( "$WORK_DIR/agent-split-rounded.png" \
     \( +clone -background black -shadow 32x16+0+18 \) +swap -background none -layers merge +repage \) \
  -gravity south -geometry +0+38 -composite \
  +profile '*' "$WORK_DIR/scene-4.png"

# Trust card.
"$MAGICK" "$WORK_DIR/dark.png" \
  -stroke '#b7cfb3' -strokewidth 4 -fill none -draw "roundrectangle 790,210 1130,350 70,70" \
  -stroke none -fill '#b7cfb3' -draw "roundrectangle 850,252 1070,308 28,28" \
  -fill '#1f3028' -draw "circle 1028,280 1052,280" \
  -font "$SERIF" -fill '#fffaf0' -pointsize 84 -gravity north -annotate +0+440 'You stay in control.' \
  -font "$SANS" -fill '#aab7af' -pointsize 34 -annotate +0+565 'Agent access is visible — and always yours to pause.' \
  +profile '*' "$WORK_DIR/scene-5.png"

# Closing card.
"$MAGICK" -size 500x110 xc:none -fill '#ff5a1f' -draw "roundrectangle 0,0 499,109 55,55" \
  -font "$SANS" -fill white -pointsize 34 -gravity center -annotate +0+0 'Visit yobro.lol  ↗' \
  +profile '*' "$WORK_DIR/cta-button.png"
"$MAGICK" "$WORK_DIR/cream.png" \
  \( "$MARK" -resize 126x126 \) -gravity north -geometry +0+180 -composite \
  -font "$SERIF" -fill '#20312a' -pointsize 102 -gravity north -annotate +0+350 'Your browser. Your bro.' \
  -font "$SANS" -fill '#6d7872' -pointsize 34 -annotate +0+505 'Preview for macOS' \
  "$WORK_DIR/cta-button.png" -gravity north -geometry +0+710 -composite \
  +profile '*' "$WORK_DIR/scene-6.png"

render_clip() {
  local scene="$1"
  local duration="$2"
  local frames="$3"
  local output="$4"
  "$FFMPEG" -hide_banner -loglevel error -y -loop 1 -i "$scene" \
    -vf "scale=3840:2160,zoompan=z='min(zoom+0.00022,1.035)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=${frames}:s=1920x1080:fps=30,format=yuv420p" \
    -t "$duration" -an -c:v libx264 -preset medium -crf 17 "$output"
}

render_clip "$WORK_DIR/scene-1.png" 3.2 96 "$WORK_DIR/clip-1.mp4"
render_clip "$WORK_DIR/scene-2.png" 4.0 120 "$WORK_DIR/clip-2.mp4"
render_clip "$WORK_DIR/scene-3.png" 4.0 120 "$WORK_DIR/clip-3.mp4"
render_clip "$WORK_DIR/scene-4.png" 4.0 120 "$WORK_DIR/clip-4.mp4"
render_clip "$WORK_DIR/scene-5.png" 3.2 96 "$WORK_DIR/clip-5.mp4"
render_clip "$WORK_DIR/scene-6.png" 3.6 108 "$WORK_DIR/clip-6.mp4"

# Crossfade all scenes. The restrained synthesized bed is original and contains no sampled audio.
"$FFMPEG" -hide_banner -loglevel error -y \
  -i "$WORK_DIR/clip-1.mp4" -i "$WORK_DIR/clip-2.mp4" -i "$WORK_DIR/clip-3.mp4" \
  -i "$WORK_DIR/clip-4.mp4" -i "$WORK_DIR/clip-5.mp4" -i "$WORK_DIR/clip-6.mp4" \
  -f lavfi -i "aevalsrc=0.020*sin(2*PI*110*t)+0.012*sin(2*PI*164.81*t)+0.009*sin(2*PI*220*t):s=48000:d=19" \
  -filter_complex "[0:v][1:v]xfade=transition=hblur:duration=0.7:offset=2.5[v1];[v1][2:v]xfade=transition=smoothleft:duration=0.7:offset=5.8[v2];[v2][3:v]xfade=transition=zoomin:duration=0.7:offset=9.1[v3];[v3][4:v]xfade=transition=circleopen:duration=0.7:offset=12.4[v4];[v4][5:v]xfade=transition=fadeslow:duration=0.7:offset=14.9[v];[6:a]lowpass=f=900,afade=t=in:st=0:d=1.2,afade=t=out:st=16.8:d=1.8[a]" \
  -map '[v]' -map '[a]' -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p \
  -c:a aac -b:a 160k -movflags +faststart -metadata title='YOBRO Promo — Mock Data' \
  -metadata comment='Uses fictional promotional data only' -shortest "$FINAL"

cp "$WORK_DIR/scene-2.png" "$POSTER"

echo "$FINAL"
echo "$POSTER"
