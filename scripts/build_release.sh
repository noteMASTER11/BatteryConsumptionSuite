#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="$ROOT_DIR/kernel"
APP_DIR="$ROOT_DIR/app"
DIST_DIR="$ROOT_DIR/dist"

echo "[1/4] Build kernel plugin"
cmake -S "$KERNEL_DIR" -B "$KERNEL_DIR/build"
cmake --build "$KERNEL_DIR/build" -j4

echo "[2/4] Copy plugin payload to app"
mkdir -p "$APP_DIR/plugin"
cp "$KERNEL_DIR/build/BatteryConsumptionKernel.skprx" "$APP_DIR/plugin/BatteryConsumptionKernel.skprx"

echo "[3/4] Build app VPK"
cmake -S "$APP_DIR" -B "$APP_DIR/build"
cmake --build "$APP_DIR/build" -j4

echo "[4/4] Prepare dist"
mkdir -p "$DIST_DIR"
cp "$APP_DIR/build/batteryconsumption.vpk" "$DIST_DIR/BatteryConsumption.vpk"
cp "$APP_DIR/build/batteryconsumption.self" "$DIST_DIR/BatteryConsumption.self"
cp "$KERNEL_DIR/build/BatteryConsumptionKernel.skprx" "$DIST_DIR/BatteryConsumptionKernel.skprx"
cp "$KERNEL_DIR/build/BatteryConsumptionKernel.skprx" "$DIST_DIR/BatteryConsumptionKernel.suprx"

echo "Done:"
ls -lh "$DIST_DIR"

