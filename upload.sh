#!/bin/sh
rsync -avzxHPS --delete-after ./ haxx@10.10.3.75:/home/haxx/matelight-controller/
