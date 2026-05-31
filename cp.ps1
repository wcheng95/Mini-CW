$dst = "C:\tmp\Mini-CW-review"
New-Item -ItemType Directory -Force $dst | Out-Null

$files = @(
  "components/app_core/app_core.c",
  "components/app_core/include/app_core.h",
  "components/audio_service/audio_service.c",
  "components/keyer_service/keyer_service.c",
  "components/ui_service/include/ui_service.h",
  "components/ui_service/private_include/ui_screen.h",
  "components/ui_service/ui_cardputer_port.cpp",
  "components/ui_service/ui_cardputer_port.h",
  "components/ui_service/ui_screen.cpp",
  "components/ui_service/ui_service.c",
  "sdkconfig"
)

foreach ($f in $files) {
  $target = Join-Path $dst $f
  New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
  Copy-Item $f $target -Force
}