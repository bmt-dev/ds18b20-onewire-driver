import time

while 1:
    with open('/dev/ds18b20', 'r') as f:
        print(f.read())
        time.sleep(5)
