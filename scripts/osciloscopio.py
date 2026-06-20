import sys
import serial
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtWidgets, QtCore

# ==========================================
# CONFIGURACIÓN DEL PUERTO SERIE
# ==========================================
PUERTO = 'COM11'
BAUDIOS = 921600
PUNTOS_PANTALLA = 400

try:
    # Abrimos el puerto serie con un timeout corto para no bloquear la interfaz
    ser = serial.Serial(PUERTO, BAUDIOS, timeout=0.05)
    print(f"Conectado exitosamente a {PUERTO}")
except Exception as e:
    print(f"Error abriendo el puerto {PUERTO}: {e}")
    sys.exit(1)

# ==========================================
# CONFIGURACIÓN DE LA INTERFAZ GRÁFICA
# ==========================================
app = QtWidgets.QApplication([])
win = pg.GraphicsLayoutWidget(show=True, title="Osciloscopio ESP32")
win.resize(800, 500)

# Creamos el lienzo del gráfico
plot = win.addPlot(title="Señal ADC - Tiempo Real")
plot.setYRange(0, 4095) # Límite exacto del ADC de 12 bits
plot.showGrid(x=True, y=True, alpha=0.5)

# Ocultamos los números del eje X ya que solo nos importa la forma de onda
plot.getAxis('bottom').setTicks([]) 

# Creamos la curva principal (color cian, grosor 2)
curve = plot.plot(pen=pg.mkPen('c', width=2))

buffer_datos = []

# ==========================================
# FUNCIÓN DE LECTURA Y ACTUALIZACIÓN
# ==========================================
def actualizar_grafico():
    global buffer_datos
    
    # Leemos todo lo que haya llegado al puerto serie
    while ser.in_waiting > 0:
        try:
            # Leemos una línea, la decodificamos y limpiamos espacios/saltos
            linea = ser.readline().decode('ascii').strip()
            
            # Si es un número válido, lo agregamos al buffer
            if linea.isdigit():
                buffer_datos.append(int(linea))
        except UnicodeDecodeError:
            pass # Ignoramos basura inicial que pueda llegar al conectar
            
    # Si acumulamos una ráfaga completa (o más) del ESP32
    if len(buffer_datos) >= PUNTOS_PANTALLA:
        # Tomamos exactamente los últimos 400 puntos para garantizar el encuadre
        ventana = buffer_datos[-PUNTOS_PANTALLA:]
        
        # Actualizamos la curva en pantalla al instante
        curve.setData(ventana)
        
        # Vaciamos el buffer para esperar la próxima ráfaga limpia
        buffer_datos.clear()

# ==========================================
# BUCLE PRINCIPAL (TIMER)
# ==========================================
# Usamos un QTimer para que revise el puerto serie cada 10 milisegundos
timer = QtCore.QTimer()
timer.timeout.connect(actualizar_grafico)
timer.start(10)

if __name__ == '__main__':
    print("Iniciando osciloscopio... (Cerrá la ventana para salir)")
    # Arrancamos el motor gráfico de Qt
    QtWidgets.QApplication.instance().exec_()
    ser.close()
    print("Puerto serie cerrado.")