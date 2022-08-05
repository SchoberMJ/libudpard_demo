# libudpard Demo

This is a package to demonstrate setting up sockets to use with libudpard (libethard).

This demo is very simple, the client serializes a cyphal message, pushes it into the libudpard tx queue, sends the messages from the queue, receives the message from the server and deserializes the message.

The server simply receives messages over a multicast socket and sends the same messages back to the sender

Build the package
```
mkdir build
cd build
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 ../
```

In separate sessions
```
# session 1
cd $project_dir/build
./client

# session 2
./server 127.A.B.C

# session 3
./server 127.X.Y.Z
```
