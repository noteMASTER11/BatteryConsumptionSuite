#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="$ROOT_DIR/kernel"
SUPRX_DIR="$ROOT_DIR/suprx"
APP_DIR="$ROOT_DIR/app"
DIST_DIR="$ROOT_DIR/dist"

echo "[1/5] Build kernel plugin"
cmake -S "$KERNEL_DIR" -B "$KERNEL_DIR/build"
cmake --build "$KERNEL_DIR/build" -j4

echo "[2/5] Build safe user SUPRX"
cmake -S "$SUPRX_DIR" -B "$SUPRX_DIR/build"
cmake --build "$SUPRX_DIR/build" -j4

echo "[3/5] Copy plugin payload to app"
mkdir -p "$APP_DIR/plugin"
cp "$KERNEL_DIR/build/BatteryConsumptionKernel.skprx" "$APP_DIR/plugin/BatteryConsumptionKernel.skprx"
cp "$SUPRX_DIR/build/BatteryConsumptionKernel.suprx" "$APP_DIR/plugin/BatteryConsumptionKernel.suprx"

echo "[4/5] Build app VPK"
cmake -S "$APP_DIR" -B "$APP_DIR/build"
cmake --build "$APP_DIR/build" -j4

echo "[5/5] Prepare dist"
mkdir -p "$DIST_DIR"
cp "$APP_DIR/build/batteryconsumption.vpk" "$DIST_DIR/BatteryConsumption.vpk"
cp "$APP_DIR/build/batteryconsumption.self" "$DIST_DIR/BatteryConsumption.self"
cp "$KERNEL_DIR/build/BatteryConsumptionKernel.skprx" "$DIST_DIR/BatteryConsumptionKernel.skprx"
cp "$SUPRX_DIR/build/BatteryConsumptionKernel.suprx" "$DIST_DIR/BatteryConsumptionKernel.suprx"

echo "Done:"
ls -lh "$DIST_DIR"
