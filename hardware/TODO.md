# Hardware TODO

## Reroute the HDMI (TMDS) tracks
- The four HDMI pairs (`HDMI_CK`, `HDMI_D0`, `HDMI_D1`, `HDMI_D2`) are routed on **In2** (layer 3) at **0.15 mm** wide.
- On In2 the nearest plane is **B.Cu**, only 0.1 mm away, so 0.15 mm traces come out at roughly **80-85 ohm differential**, not 100 ohm (hand estimate, not verified).
- The `HDMI_TMDS` net class is currently set to **0.10 mm width / 0.20 mm gap**, which should be near 100 ohm on In2. The routed tracks do not match it yet.
- To do: reroute the pairs at 0.10 / 0.20 (check in KiCad's Transmission Line calculator), or order from JLCPCB with controlled impedance (100 ohm differential) and let them adjust the widths.
- Also check: P/N length within each pair matches to about 0.5 mm; B.Cu is a solid GND pour under the whole HDMI route; GND stitching vias beside each layer-change via.
