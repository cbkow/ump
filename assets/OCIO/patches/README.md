# OCIO config patches

QCView ships Blender's stock OCIO configs with a small EDR patch on top
(2 linear-light display colorspaces + views for macOS EDR output — see
docs/hdr.md). These .patch files record that delta so the next Blender
config upgrade is mechanical:

```
cp -R /Applications/Blender.app/Contents/Resources/<ver>/datafiles/colormanagement assets/OCIO/Blender<ver>
cd assets/OCIO/Blender<ver>
patch config.ocio < ../patches/blender52-edr.patch
```

If the hunks drift on a future config, the invariants that must hold:
the EDR display colorspaces land values where 1.0 = 100-nit SDR white
(encoding: display-linear, from_display_reference via
cie_xyz_d65_interchange → Linear Rec.709 / Linear DCI-P3 D65), and the
new displays/views must be appended to the explicit active_displays /
active_views lists or OCIO hides them.

The ACES 2.0 config carries the same patch shape (display names suffixed
" - Display", views reuse ASWF's ACES 2.0 HDR transforms); no stock copy
of its base (ASWF studio-config-all-views v4.0.0) is kept locally, so
regenerate its diff from the upstream download if it ever needs a rebase.
