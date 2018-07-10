#! /bin/bash
echo "Hey, somebody called for the gidget test proc!"
echo ""
echo "this is the proc working"
echo ""
touch /tmp/itworked
echo "I just tried to touch /tmp/itworked, you know."
echo ""
echo "I am executing as `whoami` now"
echo "and I am running from `pwd` like a good little proc"
echo "I was passed $# arguments, not counting my name invocation"
echo "here they are in all their glory:"
echo $@
echo ""
echo "bye now, I'm just going to go kill myself"
exit 0
