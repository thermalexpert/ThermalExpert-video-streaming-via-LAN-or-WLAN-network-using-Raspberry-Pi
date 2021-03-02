texd-x64:
	arch | grep x86_64 # Check if x86_64
	g++ -o texd texd.cpp -I/usr/local/include/i3system -L/usr/local/lib -li3system_te_64 -I /usr/include/opencv2/ -L /usr/lib/x86_64-linux-gnu/ -lopencv_core -lopencv_highgui -lpthread -lopencv_imgproc -lconfig++

texd-armhf:
	arch | grep arm # Check if arm 
	g++ -o texd texd.cpp -I/usr/local/include/i3system -L/usr/local/lib -li3system_te_32 -I /usr/include/opencv2/ -L /usr/lib/x86_64-linux-gnu/ -lopencv_core -lopencv_highgui -lpthread -lopencv_imgproc -lconfig++

texd-armhf-opencv2:
	arch | grep arm # Check if arm 	
	g++ -o texd texd.cpp -DOPENCV2 -I/usr/local/include/i3system -L/usr/local/lib -li3system_te_32 -I /usr/include/opencv2/ -L /usr/lib/x86_64-linux-gnu/ -lopencv_core -lopencv_highgui -lpthread -lopencv_imgproc -lopencv_contrib -lconfig++

clean: 
	rm -f texd
