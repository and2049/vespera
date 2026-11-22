# Fails when design-system sources reintroduce raw colours, fonts, radii, shadows or
# accent selection instead of the tokens in include/vespera/ui_tokens.h.
# Module artwork skins (module ui.mm drawing, artwork scrims) and the token adapters
# themselves are intentionally out of scope.
if(NOT ROOT)
  message(FATAL_ERROR "ROOT is required")
endif()

set(files
  apps/macos/Sources/SettingsControls.swift
  apps/macos/Sources/AudioSettingsView.swift
  apps/macos/Sources/MIDIControls.swift
  apps/macos/Sources/TempoControls.swift
  apps/macos/Sources/TakeControls.swift
  apps/macos/Sources/ControlPill.swift
  apps/macos/Sources/ComponentGallery.swift
  apps/macos/Sources/GlobalControls.swift
  apps/macos/Sources/PresetControls.swift
  apps/macos/Sources/VesperaApp.swift
  apps/macos/Sources/PedalCatalog.swift
  apps/macos/Sources/CabinetPage.swift
  apps/macos/Sources/AmpCarousel.swift
  apps/macos/Sources/SpecialFXCatalog.swift
  apps/macos/NativeGallery/gallery.mm
  include/vespera/module_controls_macos.h)

set(patterns
  [=[Color\(white:[ ]*[0-9.]]=]
  [=[colorWithWhite:[ ]*[0-9.]]=]
  [=[colorWithSRGBRed:[ ]*[0-9.]]=]
  [=[Color\.white\.opacity\(]=]
  [=[Color\.(red|gray|orange|green)]=]
  [=[systemGreenColor]=]
  [=[\.shadow\(]=]
  [=[\.font\(\.system\(size:[ ]*[0-9]]=]
  [=[systemFontOfSize:[ ]*[0-9]]=]
  [=[cornerRadius:[ ]*[0-9]]=]
  [=[[xy]Radius:[0-9]]=])

set(failed FALSE)
foreach(relative IN LISTS files)
  set(path "${ROOT}/${relative}")
  if(NOT EXISTS "${path}")
    message(SEND_ERROR "missing scoped design-system source: ${relative}")
    set(failed TRUE)
    continue()
  endif()
  file(READ "${path}" content)
  foreach(pattern IN LISTS patterns)
    string(REGEX MATCHALL "${pattern}" hits "${content}")
    if(hits)
      list(LENGTH hits count)
      message(SEND_ERROR "${relative}: ${count} raw value(s) matching '${pattern}'; use ui_tokens.h")
      set(failed TRUE)
    endif()
  endforeach()
endforeach()

if(failed)
  message(FATAL_ERROR "UI token drift detected")
endif()
message(STATUS "UI token drift: no violations")
