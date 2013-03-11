#!/usr/bin/expect -f 
#the program file in the host PC 
set host_file        lophilo.ko    
#the password of the user in lophilo board
set lophilo_user     lophilo      
#the ip or hostname of lophilo board    
set lophilo_password lab123      
#the user name in lophilo board
set lophilo_address  cnshaxem01    
#the directory to store the program on lophilo board
set lophilo_path     ~/test         
#download program
spawn scp $host_file $lophilo_user@$lophilo_address:$lophilo_path
set timeout 5
expect "password:"
set timeout 5
send "$lophilo_password\r"
set timeout 5
send "exit\r"
expect eof
