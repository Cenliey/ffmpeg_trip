//
// Created by 15860 on 2026/6/4.
//
#include <GL/glew.h>
 #include <GLFW/glfw3.h>
 #include <iostream>

 int main() {
     if (!glfwInit()) {
         std::cerr << "GLFW init failed" << std::endl;
         return 1;
     }

     glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
     glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
     glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

     GLFWwindow* window = glfwCreateWindow(800, 600, "OpenGL Test", nullptr, nullptr);
     if (!window) {
         std::cerr << "GLFW window failed" << std::endl;
         glfwTerminate();
         return 1;
     }

     glfwMakeContextCurrent(window);

     if (glewInit() != GLEW_OK) {
         std::cerr << "GLEW init failed" << std::endl;
         return 1;
     }

     std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

     while (!glfwWindowShouldClose(window)) {
         glClear(GL_COLOR_BUFFER_BIT);
         glfwSwapBuffers(window);
         glfwPollEvents();
     }

     glfwDestroyWindow(window);
     glfwTerminate();
     return 0;
 }
