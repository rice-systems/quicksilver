/*
 * precision_test.cpp

 *
 *  Created on: Jul 13, 2016
 *      Author: Claudio Sanhueza
 *      Contact: csanhuezalobos@gmail.com
 */

#include <iostream>
#include <iomanip>
#include "kissrandom.h"
#include "annoylib.h"
#include <chrono>
#include <algorithm>
#include <map>
#include <random>

int bench(int f=100, int n=1000000){
	std::chrono::high_resolution_clock::time_point t_start, t_end;

	std::default_random_engine generator;
	std::normal_distribution<double> distribution(0.0, 1.0);

	//******************************************************
	//Building the tree
	AnnoyIndex<int, double, Angular, Kiss64Random> t = AnnoyIndex<int, double, Angular, Kiss64Random>(f);

	std::cout << "Building index ... be patient !!" << std::endl;
	std::cout << "\"Trees that are slow to grow bear the best fruit\" (Moliere)" << std::endl;



	for(int i=0; i<n; ++i){
		double *vec = (double *) malloc( f * sizeof(double) );

		for(int z=0; z<f; ++z){
			vec[z] = (distribution(generator));
		}

		t.add_item(i, vec);

		std::cout << "Loading objects ...\t object: "<< i+1 << "\tProgress:"<< std::fixed << std::setprecision(2) << (double) i / (double)(n + 1) * 100 << "%\r";

	}
	std::cout << std::endl;
	std::cout << "Building index num_trees = num_features ...";
	t_start = std::chrono::high_resolution_clock::now();
	t.build(f);
	t_end = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( t_end - t_start ).count();
	std::cout << " Done in "<< duration << " ms." << std::endl;


	std::cout << "Saving index ...";
	t.save("ann.tree");
	std::cout << " Done" << std::endl;



	//******************************************************
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


	if(argc == 1){
		f = 100;
		n = 1000000;

		feedback(f,n);

		bench(100, 1000000);
	}
	else if(argc == 3){

		f = atoi(argv[1]);
		n = atoi(argv[2]);

		feedback(f,n);

		bench(f, n);
	}
	else {
		help();
		return EXIT_FAILURE;
	}


	return EXIT_SUCCESS;
}
