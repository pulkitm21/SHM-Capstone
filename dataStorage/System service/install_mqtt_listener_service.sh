#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mqtt-listener-data.service"
SRC_SERVICE="./${SERVICE_NAME}"
DST_SERVICE="/etc/systemd/system/${SERVICE_NAME}"

if [[ ! -f "$SRC_SERVICE" ]]; then
  echo "Service file not found: $SRC_SERVICE"
  exit 1
fi

sudo cp "$SRC_SERVICE" "$DST_SERVICE"
sudo chmod 644 "$DST_SERVICE"
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl restart "$SERVICE_NAME"

echo
echo "Installed and started ${SERVICE_NAME}"
echo
echo "Useful commands:"
echo "  sudo systemctl status ${SERVICE_NAME}"
echo "  sudo journalctl -u ${SERVICE_NAME} -f"
echo "  sudo systemctl restart ${SERVICE_NAME}"
