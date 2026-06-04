# comments are with #

# what now
# this music is my fav btw


# what is this separator? must it be an actual tab?
#yes?
clean:
	rm -rf out/*

# not scalable for now but oh well
compile_out: clean
	gcc -c -Iinclude src/rand.c -o out/rand
	gcc -c -Iinclude src/pq.c -o out/pq
	gcc -c -Iinclude src/fl.c -o out/fl

# test relies on out
test: compile_out
	gcc -I include/ tests/main.c out/rand out/pq out/fl -o out/test && ./out/test

main: compile_out
	gcc -I include/ src/main.c out/rand out/pq out/fl -o ./tiltyard && ./tiltyard

