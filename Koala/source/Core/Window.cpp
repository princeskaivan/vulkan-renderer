#include <Core/Window.h>
#include <Event/ApplicationEvent.h>
#include <Event/KeyboardEvent.h>
#include <Event/MouseEvent.h>

// TODO: Refactor
#include <Renderer/VulkanContext.h>

int Window::s_windows_created_count = 0;

Window::~Window() {
	terminate_window();

	s_windows_created_count--;
	if (s_windows_created_count == 0)
		terminate_glfw();

	m_context->destroy();

	m_context.release();
}

Resolution Window::get_monitor_resolution() {
	const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

	return { (uint32_t)mode->width, (uint32_t)mode->height };
}

int Window::width() {
	glfwGetWindowSize(m_window, &m_width, &m_height);
	return m_width;
}

int Window::height() {
	glfwGetWindowSize(m_window, &m_width, &m_height);
	return m_height;
}

bool Window::is_minimized() {
	int minimized = glfwGetWindowAttrib(m_window, GLFW_ICONIFIED);

	return minimized;
}

void Window::initialize(const WindowProperties& window_props) {
	if (s_windows_created_count == 0) {
		init_glfw();
		s_windows_created_count++;
	}

	init_window(window_props);

	// TODO: Refactor
	m_context = std::make_unique<VulkanContext>();
	m_context->create(m_window);
}

void Window::on_update() {
	glfwPollEvents();
}

void Window::init_window(const WindowProperties& window_props) {
	m_width = window_props.width;
	m_height = window_props.height;

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	m_window = glfwCreateWindow(m_width, m_height, window_props.title.data(), nullptr, nullptr);
	if (!m_window)
		throw std::runtime_error("Failed to create a window");

	m_callback = window_props.callback;

	auto f = &m_callback;

	glfwSetWindowUserPointer(m_window, &m_callback);

	glfwSetWindowCloseCallback(m_window, [](GLFWwindow *window) {
		Event::event_handler_fn& callback = *(Event::event_handler_fn*)glfwGetWindowUserPointer(window);

		WindowCloseEvent e;
		callback(e);
	});

	glfwSetWindowSizeCallback(m_window, [](GLFWwindow* window, int width, int height) {
		Event::event_handler_fn& callback = *(Event::event_handler_fn*)glfwGetWindowUserPointer(window);

		if (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
			return;

		//this->m_width = width;
		//this->m_height = height;

		WindowResizeEvent e(width, height);
		callback(e);
	});

	glfwSetKeyCallback(m_window, [](GLFWwindow *window, int key, int scancode, int action, int mods) {
		Event::event_handler_fn& callback = *(Event::event_handler_fn*)glfwGetWindowUserPointer(window);

		switch (action) {
			case GLFW_PRESS:
			{
				KeyPressedEvent e(key, 0);
				callback(e);
				break;
			}
			case GLFW_RELEASE:
			{
				KeyReleasedEvent e(key);
				callback(e);
				break;
			}
			case GLFW_REPEAT:
			{
				KeyPressedEvent e(key, 1);
				callback(e);
				break;
			}
		}
	});

	glfwSetCursorPosCallback(m_window, [](GLFWwindow* window, double xpos, double ypos) {
		Event::event_handler_fn& callback = *(Event::event_handler_fn*)glfwGetWindowUserPointer(window);

		MouseMovedEvent e(xpos, ypos);
		callback(e);
	});

	glfwSetMouseButtonCallback(m_window, [](GLFWwindow* window, int button, int action, int mods) {
		Event::event_handler_fn& callback = *(Event::event_handler_fn*)glfwGetWindowUserPointer(window);

		switch (action) {
			case GLFW_PRESS: {
				MouseButtonPressedEvent e(button);
				callback(e);
				break;
			}
			case GLFW_RELEASE: {
				MouseButtonReleasedEvent e(button);
				callback(e);
				break;
			}
		}
	});
}

void Window::terminate_window() {
	glfwDestroyWindow(m_window);
}

void Window::init_glfw() {
	glfwInit();

	glfwSetErrorCallback([](int error_code, const char* description) {
		// TODO: Log Error
		std::cout << description << '\n';
	});
}

void Window::terminate_glfw() {
	glfwTerminate();
}