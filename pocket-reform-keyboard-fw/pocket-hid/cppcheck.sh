#!/bin/bash

cppcheck --check-level=exhaustive -D USBD_DESC_STR_MAX=40 -DHID_REPORT_ID= src

