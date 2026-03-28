Systemd setup for mqtt_listener_data.py

1. Edit mqtt-listener-data.service if needed:
   - User / Group
   - WorkingDirectory
   - ExecStart

2. Copy both files to your Raspberry Pi project directory.

3. Run:
   chmod +x install_mqtt_listener_service.sh
   ./install_mqtt_listener_service.sh

4. Check status:
   sudo systemctl status mqtt-listener-data.service

5. Follow logs:
   sudo journalctl -u mqtt-listener-data.service -f

Notes:
- This service starts automatically on reboot.
- It restarts automatically if the script crashes.
- Update the paths if your script is not located at:
    /home/pi/windturbine/mqtt_listener_data.py
