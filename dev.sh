make clean
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable irrigationd
sudo systemctl start irrigationd
sudo journalctl -u irrigationd -f
