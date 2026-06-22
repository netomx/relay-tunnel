//g++ gui_agent.cpp -o gui_agent.exe -lws2_32 -mwindows -O2 -static
#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <ws2tcpip.h>
#include <functional> 
#include <iomanip>    
#include <sstream>    

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

using namespace std;

// ========================================================================
// VARIABLES GLOBALES
// ========================================================================
HWND hLabel; 
HWND hButton;
SOCKET global_remote_socket = INVALID_SOCKET;
SOCKET global_local_socket = INVALID_SOCKET;

// Variables para los nuevos argumentos
bool global_start_vnc = true;
int global_local_port = 5900;


// ========================================================================
// FUNCIONES DE UTILIDAD Y RED
// ========================================================================
string generate_hardware_id() {
    DWORD volumeSerialNumber = 0;
    GetVolumeInformationA("C:\\", NULL, 0, &volumeSerialNumber, NULL, NULL, NULL, 0);
    char computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName);
    GetComputerNameA(computerName, &size);
    string raw_id = to_string(volumeSerialNumber) + "-" + string(computerName);
    hash<string> hasher;
    size_t hash_val = hasher(raw_id);
    int six_digit_id = hash_val % 1000000;
    stringstream ss;
    ss << setw(6) << setfill('0') << six_digit_id;
    return ss.str();
}

void enable_tcp_keepalive(SOCKET sock) {
    bool bOptVal = true;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&bOptVal, sizeof(bool));
}

void arrancar_vnc_silencioso() {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; 
    si.wShowWindow = SW_HIDE; 
    ZeroMemory(&pi, sizeof(pi));

    char cmd[] = "tvnserver.exe -run"; 
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void matar_vnc_silencioso() {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; 
    si.wShowWindow = SW_HIDE; 
    ZeroMemory(&pi, sizeof(pi));

    // Ejecutamos la herramienta nativa taskkill sin generar ventana
    char cmd[] = "taskkill.exe /F /IM tvnserver.exe"; 
    
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void proxy_data(SOCKET source_fd, SOCKET target_fd, string label, bool is_viewer) {
    if (source_fd == INVALID_SOCKET || target_fd == INVALID_SOCKET) return;

    char buffer[65536];
    bool is_first_packet = true;

    while (true) {
        int bytes = recv(source_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break; 
        
        if (is_viewer && is_first_packet && label.find("Remoto->Local") != string::npos) {
            string dbg(buffer, min(bytes, 12));
            for(char& c : dbg) if(c == '\n' || c == '\r') c = '.'; 
            cout << "[Debug] Primeros bytes del servidor: '" << dbg << "'" << endl;
            is_first_packet = false;
        }
        
        int sent = send(target_fd, buffer, bytes, 0);
        if (sent < 0) break;
    }
    
    shutdown(source_fd, SD_BOTH);
    shutdown(target_fd, SD_BOTH);
    closesocket(source_fd);
    if (is_viewer) cout << "[Agente] Hilo [" << label << "] finalizado." << endl;
}

void start_tunnel(SOCKET remote_fd, SOCKET local_fd, string label, bool is_viewer = false) {
    if (remote_fd != INVALID_SOCKET && local_fd != INVALID_SOCKET) {
        thread(proxy_data, remote_fd, local_fd, label + " (Remoto->Local)", is_viewer).detach();
        proxy_data(local_fd, remote_fd, label + " (Local->Remoto)", is_viewer); 
    }
}


// ========================================================================
// MODO VIEWER (Consola CLI)
// ========================================================================
void run_viewer_mode(string target_id) {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    cout << "=======================================================" << endl;
    cout << "              AGENTE VIEWER INICIADO                   " << endl;
    cout << "=======================================================\n" << endl;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    string server_host = "us2.routervpn.us";
    int server_port = 9999;
    int local_port = 6000;
    
    struct addrinfo hints, *res, *ptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(server_host.c_str(), to_string(server_port).c_str(), &hints, &res) != 0) {
        cout << "[Error] DNS fallido." << endl;
        system("pause"); return;
    }

    SOCKET remote_socket = INVALID_SOCKET;
    for(ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        remote_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (remote_socket != INVALID_SOCKET) {
            if (connect(remote_socket, (struct sockaddr*)ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
            closesocket(remote_socket);
            remote_socket = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);

    if (remote_socket == INVALID_SOCKET) {
        cout << "[Error] No se pudo conectar al servidor Relay." << endl;
        system("pause"); return;
    }
    enable_tcp_keepalive(remote_socket);

    SOCKET local_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(local_port);

    int opt = 1;
    setsockopt(local_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    if (bind(local_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) == 0) {
        listen(local_socket, 1);
        cout << "[Ok] Escuchando en puerto local: " << local_port << endl;
        cout << ">> ABRE TU CLIENTE Y CONECTA A: 127.0.0.1:" << local_port << " <<\n" << endl;

        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET vnc_socket = accept(local_socket, (struct sockaddr*)&client_addr, &client_len);

        if (vnc_socket != INVALID_SOCKET) {
            cout << "[Ok] Cliente detectado. Emparejando con ID " << target_id << "..." << endl;
            
            string req_msg = "REQ:" + target_id;
            send(remote_socket, req_msg.c_str(), (int)req_msg.length(), 0);
            
            char resp_buf[4] = {0}; 
            int total_bytes = 0;
            while (total_bytes < 3) {
                int b = recv(remote_socket, resp_buf + total_bytes, 3 - total_bytes, 0);
                if (b <= 0) break;
                total_bytes += b;
            }
            
            string response(resp_buf);
            if (response == "OK!") {
                cout << "[Ok] Puente establecido exitosamente." << endl;
                start_tunnel(remote_socket, vnc_socket, "Viewer_Bridge", true);
            } else {
                cout << "[Error] El Host rechazo la conexion. Recibido: '" << response << "'" << endl;
            }
        }
    }

    if (remote_socket != INVALID_SOCKET) closesocket(remote_socket);
    if (local_socket != INVALID_SOCKET) closesocket(local_socket);
    WSACleanup();
    cout << "\nPresiona ENTER para salir...";
    cin.get();
}


// ========================================================================
// MODO HOST (Hilo de red para la GUI)
// ========================================================================
void host_network_thread(HWND hwnd) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        SetWindowTextW(hLabel, L"Error: Winsock falló."); return;
    }

    SetWindowTextW(hLabel, L"Conectando al servidor...");

    string server_host = "us2.routervpn.us";
    int server_port = 9999;
    struct addrinfo hints, *res, *ptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(server_host.c_str(), to_string(server_port).c_str(), &hints, &res) != 0) {
        SetWindowTextW(hLabel, L"Error: DNS fallido."); WSACleanup(); return;
    }

    for(ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        global_remote_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (global_remote_socket != INVALID_SOCKET) {
            if (connect(global_remote_socket, (struct sockaddr*)ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
            closesocket(global_remote_socket);
            global_remote_socket = INVALID_SOCKET;
        }
    }
    freeaddrinfo(res);

    if (global_remote_socket == INVALID_SOCKET) {
        SetWindowTextW(hLabel, L"Error: No se pudo conectar al Relay.");
        WSACleanup(); return;
    }
    enable_tcp_keepalive(global_remote_socket);

    SetWindowTextW(hLabel, L"Registrando ID Unico...");
    string base_id_str = generate_hardware_id(); 
    int current_id_num = stoi(base_id_str);
    string my_id = base_id_str;

    while (true) {
        string reg_msg = "REG:" + my_id;
        send(global_remote_socket, reg_msg.c_str(), (int)reg_msg.length(), 0);
        
        char resp_buf[4] = {0};
        int total_bytes = 0;
        while (total_bytes < 3) {
            int b = recv(global_remote_socket, resp_buf + total_bytes, 3 - total_bytes, 0);
            if (b <= 0) break;
            total_bytes += b;
        }
        
        string response(resp_buf);
        if (response == "OK!") {
            break; 
        } else if (response == "ERR") {
            current_id_num++;
            stringstream ss;
            ss << setw(6) << setfill('0') << (current_id_num % 1000000);
            my_id = ss.str();
        } else {
            SetWindowTextW(hLabel, L"Error: Respuesta corrupta.");
            closesocket(global_remote_socket); WSACleanup(); return;
        }
    }

    wstring wide_id(my_id.begin(), my_id.end());
    wstring ui_text = L"Host Activo y Registrado\n\nTu ID de conexión es:\n" + wide_id + L"\n\nEsperando cliente...";
    SetWindowTextW(hLabel, ui_text.c_str());

    char signal_buf[6] = {0};
    int total_bytes = 0;
    while (total_bytes < 5) {
        int b = recv(global_remote_socket, signal_buf + total_bytes, 5 - total_bytes, 0);
        if (b <= 0) break;
        total_bytes += b;
    }
    
    string signal(signal_buf);
    if (signal == "START") {
        // Adaptamos el mensaje visual según los argumentos utilizados
        wstring port_str = to_wstring(global_local_port);
        wstring ui_connected = L"ID: " + wide_id + L"\n\nCliente Conectado.\nPuente activo en puerto " + port_str;
        SetWindowTextW(hLabel, ui_connected.c_str());
        
        // Solo arranca el VNC si no se pasó el parámetro --no-vnc
        if (global_start_vnc) {
            arrancar_vnc_silencioso();
            Sleep(1000); 
        }
        
        global_local_socket = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in local_service_addr;
        local_service_addr.sin_family = AF_INET;
        local_service_addr.sin_port = htons(global_local_port); 
        inet_pton(AF_INET, "127.0.0.1", &local_service_addr.sin_addr);

        if (connect(global_local_socket, (struct sockaddr*)&local_service_addr, sizeof(local_service_addr)) == 0) {
            start_tunnel(global_remote_socket, global_local_socket, "Host_Bridge");
        } else {
            wstring err_msg = L"Error: Destino local (" + port_str + L") apagado.";
            SetWindowTextW(hLabel, err_msg.c_str());
        }
    }
}


// ========================================================================
// GUI (Procedimiento de ventana)
// ========================================================================
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            hLabel = CreateWindowW(L"STATIC", L"Iniciando servicio...",
                                  WS_CHILD | WS_VISIBLE | SS_CENTER,
                                  20, 15, 240, 80, hwnd, NULL, NULL, NULL);

            hButton = CreateWindowW(L"BUTTON", L"Cerrar",
                                   WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                                   90, 115, 100, 30, hwnd, (HMENU)1, NULL, NULL);

            thread(host_network_thread, hwnd).detach();
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == 1) { SendMessage(hwnd, WM_CLOSE, 0, 0); }
            return 0;

        case WM_DESTROY:
            if (global_start_vnc) {
                matar_vnc_silencioso();
            }
            if (global_remote_socket != INVALID_SOCKET) closesocket(global_remote_socket);
            if (global_local_socket != INVALID_SOCKET) closesocket(global_local_socket);
            WSACleanup();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}


// ========================================================================
// ENTRY POINT UNIFICADO (El Switch Principal)
// ========================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    string args(lpCmdLine);
    
    // Si se ejecutó desde consola con el parámetro --connect (Modo Viewer)
    if (args.find("--connect") != string::npos) {
        string target_id = "";
        size_t pos = args.find("--connect");
        if (pos + 10 < args.length()) {
            target_id = args.substr(pos + 10);
            target_id.erase(target_id.find_last_not_of(" \n\r\t") + 1);
        }
        
        if (target_id != "") {
            run_viewer_mode(target_id);
            return 0;
        }
    }

    // Análisis de nuevos argumentos usando las macros globales de entorno (__argc y __argv)
    for (int i = 1; i < __argc; i++) {
        string arg = __argv[i];
        if (arg == "--no-vnc") {
            global_start_vnc = false;
        } else if (arg == "--port" && i + 1 < __argc) {
            global_local_port = stoi(__argv[i+1]);
            i++;
        }
    }

    // SI NO ES VIEWER, se ejecuta como Host GUI por defecto
    const wchar_t CLASS_NAME[] = L"AgenteHostClass";

    WNDCLASSW wc = { };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); 
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Relay TCP Nativo",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 200,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;
    ShowWindow(hwnd, nCmdShow);

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
