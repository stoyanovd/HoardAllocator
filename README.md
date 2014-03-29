Hoard Allocator
==============

My realization of hoard allocator.
With great help of PolarHare.
As far as I see, it works correctly.
All ideas of this allocator were read from this article: http://people.cs.umass.edu/~emery/pubs/berger-asplos2000.pdf


You may variate consts K and FRACTION - maybe it will work faster.


malloc-testing-master package contains a couple of interesting tests.



Description of some files
=========================

r and b files in root directory is compiled files of malloc-testing-master/random-alloc and malloc-testing-master/blowup respectively.
malloc-intercept.so is compiled and linked allocator library.
mymake contains commands to compile allocator and commands to run programs with this allocator.

Notice! if you want to run gedit: firstly close all gedit's windows.


