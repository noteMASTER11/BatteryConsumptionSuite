# BatteryConsumptionSuite

PS Vita project with two parts:

- `app/` - user app (`BatteryConsumption.vpk`) with text UI, history, tables, and plugin installer logic.
- `kernel/` - kernel plugin (`BatteryConsumptionKernel.skprx`) for root-level lifecycle tracking.
- `suprx/` - safe compatibility user plugin (`BatteryConsumptionKernel.suprx`) with no hooks/background work.

## Repository Layout

- `app/` - app sources, assets, and optional packaged plugin payload.
- `kernel/` - kernel plugin sources and exports.
- `suprx/` - user plugin sources and exports.
- `common/` - shared debug screen sources copied from VitaSDK samples.
- `scripts/` - helper scripts for local build/release flow.

## Build Prerequisites

- VitaSDK installed and `VITASDK` exported.
- `cmake`, `make`, and VitaSDK tools available in PATH.
- For easiest release build: WSL2 Ubuntu with VitaSDK.

## Build (manual)

1. Build kernel plugin:
   - `cd kernel`
   - `mkdir -p build && cd build`
   - `cmake .. && make -j4`
2. Build user SUPRX (optional compatibility artifact):
   - `cd suprx`
   - `mkdir -p build && cd build`
   - `cmake .. && make -j4`
3. Copy kernel plugin into app payload:
   - copy `kernel/build/BatteryConsumptionKernel.skprx` to `app/plugin/BatteryConsumptionKernel.skprx`
4. Build app VPK:
   - `cd app`
   - `mkdir -p build && cd build`
   - `cmake .. && make -j4`

Output files:

- `kernel/build/BatteryConsumptionKernel.skprx`
- `suprx/build/BatteryConsumptionKernel.suprx`
- `app/build/batteryconsumption.vpk`

## Build (script)

From repository root (`BatteryConsumptionSuite`):

- WSL/Linux:
  - `bash scripts/build_release.sh`
- PowerShell launcher for WSL:
  - `powershell -ExecutionPolicy Bypass -File scripts/build_release.ps1`

Script output:

- `dist/BatteryConsumption.vpk`
- `dist/BatteryConsumption.self`
- `dist/BatteryConsumptionKernel.skprx`
- `dist/BatteryConsumptionKernel.suprx`

## Runtime Notes

- App can auto-deploy plugin to `ur0:tai` and update `ur0:tai/config.txt`.
- Reboot is required after plugin installation.
- Logs/data are stored under `ux0:data/BatteryConsumption*`.
- For root tracking, use `BatteryConsumptionKernel.skprx` under `*KERNEL`.
- `BatteryConsumptionKernel.suprx` is intentionally safe/no-op and should not be used as a root tracker.

## GitHub Publish Quickstart

```bash
git init
git add .
git commit -m "Initial BatteryConsumptionSuite"
git branch -M main
git remote add origin <your-repo-url>
git push -u origin main
```
