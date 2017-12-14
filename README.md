# kfs
Ken's Flash File System

Created to support a fixed number of fixed sized files and a circular logging structure in the remaining disk space.  This was designed for an SD card, where different sizes of card could be used, and we wanted to make full use of the disk to extend the amount of logged data.

A traditional FAT FS was not practical in this hard-embedded environment due to the often hard-power cycle events that embedded systems are often subject to.  More advanced versions of FAT are available from some vendors that are better able to handle the hard-power cycle vent, however their flash/ram and CPU requirements were too great.  

This is a very specific purposed filesystem that was safe from abrupt power loss, very low ram and flash requirements and very quick to use.  To gain these features, we lost the ability to read the card from a standard PC without a special driver.  The files were recoverable from the web interface of the device and by a special application written to read/write this filesystem.
