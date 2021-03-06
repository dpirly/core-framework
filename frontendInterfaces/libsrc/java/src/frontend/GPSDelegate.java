/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file
 * distributed with this source distribution.
 *
 * This file is part of REDHAWK frontendInterfaces.
 *
 * REDHAWK frontendInterfaces is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * REDHAWK frontendInterfaces is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */
package frontend;

import FRONTEND.GPSInfo;
import FRONTEND.GpsTimePos;
import FRONTEND.FrontendException;
import FRONTEND.BadParameterException;
import FRONTEND.NotSupportedException;

public interface GPSDelegate {

    public GPSInfo get_gps_info(String port_name) throws FrontendException, BadParameterException, NotSupportedException;

    public void set_gps_info(String port_name, GPSInfo data) throws FrontendException, BadParameterException, NotSupportedException;

    public GpsTimePos get_gps_time_pos(String port_name) throws FrontendException, BadParameterException, NotSupportedException;

    public void set_gps_time_pos(String port_name, GpsTimePos data) throws FrontendException, BadParameterException, NotSupportedException;

}
