#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <algorithm>

using namespace std;

unordered_map<string, int> active_sessions;
mutex map_mutex;

string extract_id(string message) {
    string content;
    if (message.length() >= 4 && (message.substr(0, 4) == "REG:" || message.substr(0, 4) == "REQ:")) {
        content = message.substr(4);
    } else {
        content = message;
    }
    size_t last_dash = content.find_last_of('-');
    if (last_dash != string::npos) {
        return content.substr(last_dash + 1);
    }
    return content;
}

void forward_data(int source_fd, int target_fd, string label) {
    char buffer[65536];
    while (true) {
        int bytes = recv(source_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            if (bytes == 0) {
                cout << "[Relay] [" << label << "] Conexión cerrada limpiamente por el origen." << endl;
            } else {
                cout << "[Relay] [" << label << "] Error en recv: " << strerror(errno) << " (Código: " << errno << ")" << endl;
            }
            break;
        }

        int sent = send(target_fd, buffer, bytes, MSG_NOSIGNAL);
        if (sent < 0) {
            cout << "[Relay] [" << label << "] Error en send hacia destino: " << strerror(errno) << endl;
            break;
        }
    }
    
    shutdown(source_fd, SHUT_RDWR);
    shutdown(target_fd, SHUT_RDWR);
    close(source_fd);
    cout << "[Relay] Hilo " << label << " finalizado." << endl;
}

void handle_client(int client_fd) {
    int opt = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    while (true) {
        char buffer[1024];
        int bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            close(client_fd);
            return;
        }
        
        buffer[bytes_received] = '\0';
        string message(buffer);

        if (message.find("REG:") == 0) {
            string id = extract_id(message);
            bool success = false;
            
            {
                lock_guard<mutex> lock(map_mutex);
                if (active_sessions.count(id) == 0) {
                    active_sessions[id] = client_fd;
                    success = true;
                }
            }
            
            if (success) {
                send(client_fd, "OK!", 3, MSG_NOSIGNAL);
                cout << "[Registro] Cliente Host registrado con ID: " << id << endl;
                
                // --- INICIO DEL HILO GUARDIÁN ---
                while (true) {
                    {
                        lock_guard<mutex> lock(map_mutex);
                        // 1. Si el ID ya no está en el mapa, significa que un Viewer se conectó 
                        // y se robó este socket. Nuestro trabajo terminó.
                        if (active_sessions.count(id) == 0) return; 
                    }

                    char dummy;
                    // 2. Intentamos leer 1 byte espiando (MSG_PEEK) y sin bloquearnos (MSG_DONTWAIT)
                    int check = recv(client_fd, &dummy, 1, MSG_PEEK | MSG_DONTWAIT);
                    
                    // Si check es 0 (cierre limpio) o un error fatal (caída de red)
                    if (check == 0 || (check < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        {
                            lock_guard<mutex> lock(map_mutex);
                            active_sessions.erase(id); // Liberamos el ID al instante
                        }
                        close(client_fd);
                        cout << "[Relay] Host " << id << " cerró el programa. ID liberado." << endl;
                        return;
                    }
                    
                    sleep(1); // Descansar 1 segundo para no consumir CPU
                }
                // --- FIN DEL HILO GUARDIÁN ---

            } else {
                send(client_fd, "ERR", 3, MSG_NOSIGNAL);
                cout << "[Registro] Host intento usar ID ocupado: " << id << ". Pidiendo otro..." << endl;
            }
            
        } else if (message.find("REQ:") == 0) {
            string target_id = extract_id(message);
            int target_fd = -1;
            
            {
                lock_guard<mutex> lock(map_mutex);
                if (active_sessions.count(target_id)) {
                    target_fd = active_sessions[target_id];
                    // Borramos el ID del mapa. ¡Esto le avisa al hilo guardián que se retire!
                    active_sessions.erase(target_id); 
                }
            }
            
            if (target_fd != -1) {
                cout << "[Solicitud] Cliente Viewer emparejado con Host ID: " << target_id << endl;
                send(client_fd, "OK!", 3, MSG_NOSIGNAL);

                if (send(target_fd, "START", 5, MSG_NOSIGNAL) < 0) {
                    cout << "[Error] No se pudo enviar START al Host: " << strerror(errno) << endl;
                    close(client_fd);
                    close(target_fd);
                    return;
                }
                
                thread(forward_data, target_fd, client_fd, "Host->Viewer").detach();
                forward_data(client_fd, target_fd, "Viewer->Host"); 
            } else {
                cout << "[Error] ID " << target_id << " no encontrado o ya emparejado." << endl;
                send(client_fd, "ERR", 3, MSG_NOSIGNAL); 
                close(client_fd);
            }
            return; 
        } else {
            close(client_fd);
            return;
        }
    }
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int port = 9999;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    cout << "Servidor Relay iniciado en el puerto " << port << "..." << endl;

    while (true) {
        socklen_t addr_len = sizeof(address);
        new_socket = accept(server_fd, (struct sockaddr *)&address, &addr_len);
        if (new_socket < 0) continue;
        thread(handle_client, new_socket).detach();
    }
    return 0;
}
