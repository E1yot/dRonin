/**
  ******************************************************************************
  * @addtogroup PIOS PIOS Core hardware abstraction layer
  * @{
  * @addtogroup PIOS_MPXV5004 MPXV5004 Functions
  * @brief Hardware functions to deal with the DIYDrones airspeed kit, using MPXV5004
  * @{
  *
  * @file       pios_mpxv5004.h
  * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
  * @brief      ETASV3 Airspeed Sensor Driver
  * @see        The GNU Public License (GPL) Version 3
  *
  ******************************************************************************/
/* 
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 3 of the License, or 
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#ifndef PIOS_MPXV5004_H__

uint16_t PIOS_MPXV5004_Measure(uint8_t airspeedADCPin);
uint16_t PIOS_MPXV5004_Calibrate(uint8_t airspeedADCPin, uint16_t calibrationCount);
void PIOS_MPXV5004_UpdateCalibration(uint16_t zeroPoint);
float PIOS_MPXV5004_ReadAirspeed (uint8_t airspeedADCPin);

#endif // PIOS_MPXV5004_H__
