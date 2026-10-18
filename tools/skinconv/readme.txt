To convert an existing skin to work online, you will need to install python.

After installing python, "cd" to this directory (where the scripts are in). Create a folder, for example "malon" in
this directory. Place all the otr/o2r for the skin inside the folder "malon". Then you would run the script like this:

python skinconv.py malon -o malon.o2r

"malon" is the name of the folder you placed the files in, malon.o2r is the new output file.
In config.json you would add this to "skins":

{ "displayName": "My malon skin", "referenceName": "malon", "pitch": 1.15}

A pitch of 1.15 for female, 1.0 for original link voice. The server will need to be restarted. 

The players will need to place "malon.o2r" in their mods directory. 

OR

You may also run a HTTP server (it must not force redirect to HTTPS, which is not supported). The player will set their "Resource Server" to this HTTP server
where it can automatically download the malon.o2r file

Example if the player set their resource server to "http://awu.fks.mybluehost.me/" then the mod file should be available at "http://awu.fks.mybluehost.me/malon.o2r"