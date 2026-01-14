#include <stdio.h>

int main() {
    // Print a welcome message
    printf("Hello, World!\n");
    
    // Declare and initialize variables
    int age = 25;
    char name[] = "John";
    float height = 5.9;
    
    // Print variable values
    printf("Name: %s\n", name);
    printf("Age: %d\n", age);
    printf("Height: %.1f feet\n", height);
    
    // Simple calculation
    int sum = 10 + 20;
    printf("10 + 20 = %d\n", sum);
    
    // Loop example
    printf("Counting from 1 to 5:\n");
    for (int i = 1; i <= 5; i++) {
        printf("%d ", i);
    }
    printf("\n");
    
    return 0;
}
