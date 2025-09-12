sudo cp /etc/systemd/system/irrigationd.service .
sed -i 's/10.0.0.63/127.0.0.1/g' irrigationd.service
