/*
 * precision_test.cpp

 *
 *  Created on: Jul 13, 2016
 *      Author: Claudio Sanhueza
 *      Contact: csanhuezalobos@gmail.com
 */

#include <iostream>
#include <iomanip>
#include "./kissrandom.h"
#include "./annoylib.h"
#include <chrono>
#include <algorithm>
#include <map>
#include <random>

int bench(int f=100, int n=1000000, int query_n=300000){
	std::chrono::high_resolution_clock::time_point t_start, t_end;

	std::default_random_engine generator;
	std::normal_distribution<double> distribution(0.0, 1.0);

	//******************************************************
	//Building the tree
	AnnoyIndex<int, double, Angular, Kiss64Random> t = AnnoyIndex<int, double, Angular, Kiss64Random>(f);

	std::cout << "Loading index ..." << std::endl;
	// std::cout << "\"Trees that are slow to grow bear the best fruit\" (Moliere)" << std::endl;

	// t_start = std::chrono::high_resolution_clock::now();
	// t.build(2 * f);
	// t_end = std::chrono::high_resolution_clock::now();
	// auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( t_end - t_start ).count();
	// std::cout << " Done in "<< duration << " ms." << std::endl;


	// std::cout << "Saving index ...";
	t.load("ann.tree", true);
	std::cout << "Pre-touching Done" << std::endl;

	// std::cout << "\nDone" << std::endl;
	return 0;
}


void help(){
	std::cout << "Annoy Precision C++ example" << std::endl;
	std::cout << "Usage:" << std::endl;
	std::cout << "(default)		./precision" << std::endl;
	std::cout << "(using parameters)	./precision num_features num_nodes" << std::endl;
	std::cout << std::endl;
}

void feedback(int f, int n){
	std::cout<<"Runing precision example with:" << std::endl;
	std::cout<<"num. features: "<< f << std::endl;
	std::cout<<"num. nodes: "<< n << std::endl;
	std::cout << std::endl;
}


int main(int argc, char **argv) {
	int f, n;

	f = 100;
	n = 1000000;
	feedback(f,n);


	int query_n = 300000;
	if(argc == 2)
		query_n = atoi(argv[1]);

	std::cout << "query number: " << query_n << std::endl;
	bench(f, n, query_n);


	return EXIT_SUCCESS;
}
