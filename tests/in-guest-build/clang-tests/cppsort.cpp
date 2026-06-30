// EXPECT: CLANGPP-SORTED 1 2 3 4 5
#include <iostream>
#include <vector>
#include <algorithm>
int main(){ std::vector<int> v{3,1,4,2,5}; std::sort(v.begin(), v.end());
  std::cout << "CLANGPP-SORTED"; for (int x : v) std::cout << " " << x; std::cout << "\n"; return 0; }
