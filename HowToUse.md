# HOW TO USE #
### Requirement ###
  * Two computers
    * One of which get processor supporting Intel VT, we call this Guest Machine(Rootkit run on it)
    * The other one is Host Machine, which monitors the Guest Machine.
  * A Serial Cable :)
  * GRUB Installed
  * Guest OS: Theoretically BitVisor can support any guest OS, but i recommend Windows(2K, XP, 2003). I had crash experience under Ubuntu.
  * Compile Environment: Any Linux with GNU make and gcc would be ok, but I only tested under ubuntu.
  * Something that could monitor the serial port installed on Host Machine. I recommend GTKTerm

### Steps ###
  1. Connect the Guest Machine and Host Machine with serial cable
  1. Checkout the code from [the repository](http://rkanalyzer.googlecode.com/svn/trunk/)
  1. Start a terminal, enter the root directory of the source code
  1. You could use ./config.sh to do some configuration before make. But make sure the "TTY\_SERIAL" and "RK\_ANALYZER" label are checked.
  1. type make and get a cup of coffee.
  1. if nothing get wrong here, you should get a executable named bitvisor.elf. Copy it to /boot/ on the Guest Machine
  1. Modify menu.lst. Add following:
```
title BitVisor
root (hd0,0)
kernel /boot/bitvisor.elf
```
> Make sure you substitute the (hd0,0) with proper location on your system.


### Usage ###
  * Start serial port monitor(i.e. GTKTerm) on Host Machine
  * Boot the Guest Machine, first select "BitVisor" in the GRUB list, then bitvisor will boot GRUB again, so you could select the operating system you want this time. If All things got right, you would see the following in GTKTerm:
> ![http://rkanalyzer.googlecode.com/svn/wiki/Screenshot-GtkTerm.png](http://rkanalyzer.googlecode.com/svn/wiki/Screenshot-GtkTerm.png)
  * Now you get into the guest OS. Build [Probes](http://code.google.com/p/rkanalyzer/wiki/Probes) corresponding to the guest OS and run the probe. Then you can test with rootkits!