/*
Copyright (c) 2014, Jorge Rodriguez, bs.vino@gmail.com

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
3. All advertising materials mentioning features or use of this software must display the following acknowledgement:
   This product includes software developed by Jorge Rodriguez.
4. Neither the name of the Jorge Rodriguez nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY JORGE RODRIGUEZ ''AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JORGE RODRIGUEZ BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "viewback.h"

#include <malloc.h>
#include <time.h>

#include "viewback_shared.h"
#include "viewback_internal.h"

void vb_config_initialize(vb_config_t* config)
{
	memset(config, 0, sizeof(vb_config_t));

	config->port = VB_DEFAULT_PORT;
	config->max_connections = 4;
}

size_t vb_config_get_memory_required(vb_config_t* config)
{
	if (!config)
		return 0;

	if (config->num_data_registrations <= 0)
		return 0;

	return
		sizeof(vb_t) +
		config->num_data_registrations * sizeof(vb_data_registration_t) +
		config->max_connections * sizeof(vb_connection_t);
}

static vb_t* VB;

int vb_config_install(vb_config_t* config, void* memory, size_t memory_size)
{
	if (!config)
		return 0;

	if (!memory)
		return 0;

	// Indicates there was a problem in vb_config_get_memory_required() which didn't get caught.
	if (memory_size == 0)
		return 0;

	if (memory_size < vb_config_get_memory_required(config))
		return 0;

	if (VB->server_active)
		return 0;

	VB = (vb_t*)memory;

	VB->config = *config;

	VB->registrations = (vb_data_registration_t*)((char*)memory + sizeof(vb_t));
	VB->next_registration = 0;

	VB->connections = (vb_connection_t*)((char*)memory + sizeof(vb_t) + sizeof(vb_data_registration_t)*config->num_data_registrations);

	for (int i = 0; i < config->max_connections; i++)
		VB->connections[i].socket = VB_INVALID_SOCKET;

	VB->server_active = false;

	return 1;
}

int vb_data_register(const char* name, vb_data_type_t type, /*out*/ vb_data_handle_t* handle)
{
	if (!name)
		return 0;

	if (!name[0])
		return 0;

	if (!handle)
		return 0;

	if (VB->next_registration >= VB->config.num_data_registrations)
		return 0;

	if (VB->server_active)
		return 0;

	*handle = (vb_data_handle_t)VB->next_registration;

	VB->registrations[VB->next_registration].name = name;
	VB->registrations[VB->next_registration].type = type;

	VB->next_registration++;

	return 1;
}

int vb_server_create()
{
	if (VB->server_active)
		return 0;

	VB->multicast_socket = socket(AF_INET, SOCK_DGRAM, 0);

	CCleanupSocket mc(VB->multicast_socket);

	if (!vb_valid_socket(VB->multicast_socket))
		return 0;

	memset(&VB->multicast_addr, 0, sizeof(VB->multicast_addr));
	VB->multicast_addr.sin_family = AF_INET;
	VB->multicast_addr.sin_addr.s_addr = inet_addr(VB_DEFAULT_MULTICAST_ADDRESS);
	VB->multicast_addr.sin_port = htons(VB->config.port);
	VB->last_multicast = 0;

	VB->tcp_socket = socket(AF_INET, SOCK_STREAM, 0);

	CCleanupSocket tc(VB->tcp_socket);

	if (!vb_valid_socket(VB->tcp_socket))
		return 0;

	{
		int on = 1;
		int off = 0;
		setsockopt(VB->tcp_socket, SOL_SOCKET, SO_REUSEADDR, (const char*) &on, sizeof(on));
		setsockopt(VB->tcp_socket, SOL_SOCKET, SO_LINGER, (const char*) &off, sizeof(off));
	}

#ifdef __APPLE__
	/* Don't generate SIGPIPE when writing to dead socket, we check all writes. */
	signal(SIGPIPE, SIG_IGN);
#endif

	if (vb_set_blocking(VB->tcp_socket, 0) != 0)
		return 0;

	struct sockaddr_in tcp_addr;
	memset(&tcp_addr, 0, sizeof(tcp_addr));
	tcp_addr.sin_family = AF_INET;
	tcp_addr.sin_addr.s_addr = INADDR_ANY;
	tcp_addr.sin_port = htons(VB->config.port);

	if (bind(VB->tcp_socket, (struct sockaddr*) &tcp_addr, sizeof tcp_addr) != 0)
	{
		int error = vb_socket_error();
		return 0;
	}

	if (listen(VB->tcp_socket, SOMAXCONN) != 0)
		return 0;

	VBPrintf("Viewback server created on %d.%d.%d.%d:%d (%u).\n",
		tcp_addr.sin_addr.S_un.S_un_b.s_b1, tcp_addr.sin_addr.S_un.S_un_b.s_b1, tcp_addr.sin_addr.S_un.S_un_b.s_b1, tcp_addr.sin_addr.S_un.S_un_b.s_b1,
		tcp_addr.sin_port, tcp_addr.sin_addr.S_un.S_addr
		);

	mc.Success();
	tc.Success();

	VB->server_active = true;

	return 1;
}

void vb_server_shutdown()
{
	if (!VB->server_active)
		return;

	vb_close_socket(VB->tcp_socket);
	vb_close_socket(VB->multicast_socket);

	VB->server_active = false;
}

size_t vb_write_length_prepended_message(struct Packet *_Packet, void *_buffer, size_t length, size_t(*serialize)(struct Packet *_Packet, void *_buffer, size_t length))
{
	size_t serialized_length = (*serialize)(_Packet, (void*)((size_t)_buffer + sizeof(size_t)), length);

	// Packet_alloca() automatically adds sizeof(size_t) bytes to the length of the packet requested, specifically to make room for this.
	size_t network_length = htonl(serialized_length);
	memcpy(_buffer, &network_length, sizeof(network_length));

	return serialized_length + sizeof(network_length);
}

void vb_server_update()
{
	if (!VB->server_active)
		return;

	time_t current_time;
	time(&current_time);

	// Advertise ourselves once per second.
	if (current_time > VB->last_multicast)
	{
		const char message[] = "VB: HELLO WORLD"; // TODO: put game info here.
		sendto(VB->multicast_socket, (const char*)message, sizeof(message), 0, (struct sockaddr *)&VB->multicast_addr, sizeof(VB->multicast_addr));

		VB->last_multicast = current_time;
	}

	fd_set read_fds;

	FD_ZERO(&read_fds);

	int current_connections = 0;
	for (int i = 0; i < VB->config.max_connections; i++)
	{
		if (VB->connections[i].socket != VB_INVALID_SOCKET)
			current_connections++;
	}

	vb_socket_t max_socket = 0;

	if (current_connections < VB->config.max_connections)
	{
		FD_SET(VB->tcp_socket, &read_fds);
		max_socket = VB->tcp_socket;
	}

	for (int i = 0; i < VB->config.max_connections; ++i)
	{
		vb_socket_t socket = VB->connections[i].socket;
		if (VB->connections[i].socket == VB_INVALID_SOCKET)
			continue;

		FD_SET(socket, &read_fds);

		if (socket > max_socket)
			max_socket = socket;
	}

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 5;

	int err = select((int) (max_socket + 1), &read_fds, nullptr, nullptr, &timeout);

	if (FD_ISSET(VB->tcp_socket, &read_fds))
	{
		// We have an incoming connection.

		VBPrintf("Incoming connection... ");

		char VB_ALIGN(8) client_addr[64];
		vb_socklen_t client_addr_len = sizeof(client_addr);
		vb_socket_t incoming_socket = accept(VB->tcp_socket, (struct sockaddr*) &client_addr[0], &client_addr_len);

		if (vb_valid_socket(incoming_socket))
		{
			CCleanupSocket c(incoming_socket);

			int open_socket = -1;
			for (int i = 0; i < VB->config.max_connections; i++)
			{
				if (VB->connections[i].socket == VB_INVALID_SOCKET)
				{
					open_socket = i;
					break;
				}
			}

			VBAssert(open_socket >= 0);

			if (open_socket >= 0)
			{
				VB->connections[open_socket].socket = incoming_socket;

				if (vb_set_blocking(incoming_socket, 0) == 0)
				{
					int off = 0;
					setsockopt(incoming_socket, SOL_SOCKET, SO_LINGER, (const char*) &off, sizeof(int));

					c.Success();
				}
			}
		}

		if (vb_valid_socket(incoming_socket))
			VBPrintf("Successful. Socket: %d\n", incoming_socket);
		else
			VBPrintf("Dropped.\n");
	}

	for (int i = 0; i < VB->config.max_connections; i++)
	{
		if (!vb_valid_socket(VB->connections[i].socket))
			continue;

		if (FD_ISSET(VB->connections[i].socket, &read_fds))
		{
			char mesg[1024];

			int n = recv(VB->connections[i].socket, mesg, sizeof(mesg), 0);

			if (!n)
			{
				VB->connections[i].socket = VB_INVALID_SOCKET;
				continue;
			}
			else if (n == sizeof(mesg))
			{
				// We read the whole damn thing? Shouldn't ever happen, but ignore.
				VBAssert(false);
				continue;
			}

			if (strcmp(mesg, "registrations") == 0)
			{
				VBPrintf("Sending registrations to %d.\n", VB->connections[i].socket);

				struct Packet packet;
				struct DataDescription* descriptions = (struct DataDescription*)alloca(VB->next_registration * sizeof(struct DataDescription));

				Packet_initialize_registrations(&packet, descriptions, VB->next_registration);

				size_t message_predicted_length = Packet_get_message_size(&packet);
				void* message = Packet_alloca(message_predicted_length);

				size_t message_actual_length = vb_write_length_prepended_message(&packet, message, message_predicted_length, &Packet_serialize);

				if (message_actual_length)
					send(VB->connections[i].socket, (const char*)message, message_actual_length, 0);
			}
		}
	}
}

void vb_send_to_all(void* message, size_t message_length)
{
	for (int i = 0; i < VB->config.max_connections; i++)
	{
		if (VB->connections[i].socket == VB_INVALID_SOCKET)
			continue;

		int bytes_sent = send(VB->connections[i].socket, (const char*)message, message_length, 0);

		if (bytes_sent < 0)
		{
			VBPrintf("Error sending to %d, disconnected.\n", VB->connections[i].socket);
			VB->connections[i].socket = VB_INVALID_SOCKET;
		}
		else
		{
			VBPrintf("Sent %d bytes to %d (msg length %d).\n", bytes_sent, VB->connections[i].socket, message_length);
		}
	}
}

int vb_data_send_int(vb_data_handle_t handle, int value)
{
	if (handle < 0)
		return 0;

	if (handle >= VB->next_registration)
		return 0;

	if (VB->registrations[handle].type != VB_DATATYPE_INT)
		return 0;

	if (!VB->server_active)
		return 0;

	struct Packet packet;
	struct Data data;
	Packet_initialize_data(&packet, &data, VB_DATATYPE_INT);

	data._handle = handle;
	data._data_int = value;

	size_t message_predicted_length = Packet_get_message_size(&packet);
	void* message = Packet_alloca(message_predicted_length);

	size_t message_actual_length = vb_write_length_prepended_message(&packet, message, message_predicted_length, &Packet_serialize);

	if (!message_actual_length)
		return 0;

	vb_send_to_all(message, message_actual_length);

	return 1;
}

int vb_data_send_float(vb_data_handle_t handle, float value)
{
	if (handle < 0)
		return 0;

	if (handle >= VB->next_registration)
		return 0;

	if (VB->registrations[handle].type != VB_DATATYPE_FLOAT)
		return 0;

	if (!VB->server_active)
		return 0;

	struct Packet packet;
	struct Data data;
	Packet_initialize_data(&packet, &data, VB_DATATYPE_FLOAT);

	data._handle = handle;
	data._data_float = value;

	size_t message_predicted_length = Packet_get_message_size(&packet);
	void* message = Packet_alloca(message_predicted_length);

	size_t message_actual_length = vb_write_length_prepended_message(&packet, message, message_predicted_length, &Packet_serialize);

	if (!message_actual_length)
		return 0;

	vb_send_to_all(message, message_actual_length);

	return 1;
}

int vb_data_send_vector(vb_data_handle_t handle, float x, float y, float z)
{
	if (handle < 0)
		return 0;

	if (handle >= VB->next_registration)
		return 0;

	if (VB->registrations[handle].type != VB_DATATYPE_VECTOR)
		return 0;

	if (!VB->server_active)
		return 0;

	struct Packet packet;
	struct Data data;
	Packet_initialize_data(&packet, &data, VB_DATATYPE_VECTOR);

	data._handle = handle;
	data._data_float_x = x;
	data._data_float_y = y;
	data._data_float_z = z;

	size_t message_predicted_length = Packet_get_message_size(&packet);
	void* message = Packet_alloca(message_predicted_length);

	size_t message_actual_length = vb_write_length_prepended_message(&packet, message, message_predicted_length, &Packet_serialize);

	if (!message_actual_length)
		return 0;

	vb_send_to_all(message, message_actual_length);

	return 1;
}






/* ====================================== PROTOBUF GENERATOR ====================================== */
/* This code unscrupulously stolen from http://code.google.com/p/protobuf-embedded-c/ 
which is under the http://www.apache.org/licenses/LICENSE-2.0 Apache license. */

int write_raw_byte(char value, void *_buffer, int offset)
{
	*((char *)_buffer + offset) = value;
	return ++offset;
}

int write_raw_bytes(const char *bytes, int bytes_size, void *_buffer, int offset)
{
	int i; 
	for (i = 0; i < bytes_size; ++ i)
	{
		offset = write_raw_byte((char)*(bytes + i), _buffer, offset);
	}

	return offset;
}

int write_raw_varint32(unsigned long value, void *_buffer, int offset)
{
	while (1)
	{
		if ((value & ~0x7F) == 0)
		{
			offset = write_raw_byte((char)value, _buffer, offset);
			return offset;
		}
		else
		{
			offset = write_raw_byte((char)((value & 0x7F) | 0x80), _buffer, offset);
			value = value >> 7;
		}
	}
	return offset;
}

int write_raw_little_endian32(unsigned long value, void *_buffer, int offset)
{
	offset = write_raw_byte((char)((value      ) & 0xFF), _buffer, offset);
	offset = write_raw_byte((char)((value >>  8) & 0xFF), _buffer, offset);
	offset = write_raw_byte((char)((value >> 16) & 0xFF), _buffer, offset);
	offset = write_raw_byte((char)((value >> 24) & 0xFF), _buffer, offset);

	return offset;
}

int vb_data_type_t_write_with_tag(vb_data_type_t *_vb_data_type_t, void *_buffer, int offset, int tag)
{
	/* Write tag.*/
	offset = write_raw_varint32((tag<<3)+0, _buffer, offset);
	/* Write content.*/
	offset = write_raw_varint32(*_vb_data_type_t, _buffer, offset);

	return offset;
}

int Data_write(struct Data *_Data, void *_buffer, int offset)
{
	/* Always write the handle */
	offset = write_raw_varint32((1<<3)+0, _buffer, offset);
	offset = write_raw_varint32(_Data->_handle, _buffer, offset);

	if (_Data->_type == VB_DATATYPE_INT)
	{
		offset = write_raw_varint32((3<<3)+0, _buffer, offset);
		offset = write_raw_varint32(_Data->_data_int, _buffer, offset);
	}

	if (_Data->_type == VB_DATATYPE_FLOAT)
	{
		unsigned long *data_float_ptr = (unsigned long *)&_Data->_data_float;

		offset = write_raw_varint32((4<<3)+5, _buffer, offset);
		offset = write_raw_little_endian32(*data_float_ptr, _buffer, offset);
	}

	if (_Data->_type == VB_DATATYPE_VECTOR)
	{
		unsigned long *data_float_x_ptr = (unsigned long *)&_Data->_data_float_x;
		unsigned long *data_float_y_ptr = (unsigned long *)&_Data->_data_float_y;
		unsigned long *data_float_z_ptr = (unsigned long *)&_Data->_data_float_z;

		offset = write_raw_varint32((5<<3)+5, _buffer, offset);
		offset = write_raw_little_endian32(*data_float_x_ptr, _buffer, offset);

		offset = write_raw_varint32((6<<3)+5, _buffer, offset);
		offset = write_raw_little_endian32(*data_float_y_ptr, _buffer, offset);

		offset = write_raw_varint32((7<<3)+5, _buffer, offset);
		offset = write_raw_little_endian32(*data_float_z_ptr, _buffer, offset);
	}

	return offset;
}

int Data_write_delimited_to(struct Data *_Data, void *_buffer, int offset)
{
	int i, shift, new_offset, size;

	new_offset = Data_write(_Data, _buffer, offset);
	size = new_offset - offset;
	shift = (size > 127) ? 2 : 1;
	for (i = new_offset - 1; i >= offset; -- i)
		*((char *)_buffer + i + shift) = *((char *)_buffer + i);

	write_raw_varint32((unsigned long) size, _buffer, offset);

	return new_offset + shift;
}

int Data_write_with_tag(struct Data *_Data, void *_buffer, int offset, int tag)
{
	/* Write tag.*/
	offset = write_raw_varint32((tag<<3)+2, _buffer, offset);
	/* Write content.*/
	offset = Data_write_delimited_to(_Data, _buffer, offset);

	return offset;
}

int DataDescription_write(struct DataDescription *_DataDescription, void *_buffer, int offset) {
	/* Write content of each message element.*/
	/* Write the optional attribute only if it is different than the default value. */
	if (_DataDescription->_field_name_len != 1 || _DataDescription->_field_name[0] != '0')
	{
		offset = write_raw_varint32((1<<3)+2, _buffer, offset);
		offset = write_raw_varint32(_DataDescription->_field_name_len, _buffer, offset);
		offset = write_raw_bytes(_DataDescription->_field_name, _DataDescription->_field_name_len, _buffer, offset);
	}
	
	/* Write the optional attribute only if it is different than the default value. */
	if (_DataDescription->_type != 0)
	{
		offset = vb_data_type_t_write_with_tag(&_DataDescription->_type, _buffer, offset, 2);
	}
	
	/* Write the optional attribute only if it is different than the default value. */
	if(_DataDescription->_handle != 0)
	{
		offset = write_raw_varint32((3<<3)+0, _buffer, offset);
		offset = write_raw_varint32(_DataDescription->_handle, _buffer, offset);
	}
	
	return offset;
}

int DataDescription_write_delimited_to(struct DataDescription *_DataDescription, void *_buffer, int offset)
{
	int i, shift, new_offset, size;

	new_offset = DataDescription_write(_DataDescription, _buffer, offset);
	size = new_offset - offset;
	shift = (size > 127) ? 2 : 1;
	for (i = new_offset - 1; i >= offset; -- i)
		*((char *)_buffer + i + shift) = *((char *)_buffer + i);

	write_raw_varint32((unsigned long) size, _buffer, offset);         

	return new_offset + shift;
}

int DataDescription_write_with_tag(struct DataDescription *_DataDescription, void *_buffer, int offset, int tag)
{
	/* Write tag.*/
	offset = write_raw_varint32((tag<<3)+2, _buffer, offset);
	/* Write content.*/
	offset = DataDescription_write_delimited_to(_DataDescription, _buffer, offset);

	return offset;
}

int Packet_write(struct Packet *_Packet, void *_buffer, int offset)
{
	int data_descriptions_cnt;

	/* Write content of each message element.*/
	/* Write the optional attribute only if it is different than the default value. */
	if (_Packet->_data)
	{
		offset = Data_write_with_tag(_Packet->_data, _buffer, offset, 1);
	}

	for (data_descriptions_cnt = 0; data_descriptions_cnt < _Packet->_data_descriptions_repeated_len; ++ data_descriptions_cnt)
	{
		offset = DataDescription_write_with_tag(&_Packet->_data_descriptions[data_descriptions_cnt], _buffer, offset, 2);
	}

	return offset;
}

void Packet_initialize_data(struct Packet* packet, struct Data* data, vb_data_type_t type)
{
	memset(packet, 0, sizeof(struct Packet));

	packet->_data = data;

	memset(data, 0, sizeof(struct Data));

	data->_type = type;
}

void Packet_initialize_registrations(struct Packet* packet, struct DataDescription* data_reg, size_t registrations)
{
	memset(packet, 0, sizeof(struct Packet));

	packet->_data_descriptions = data_reg;
	packet->_data_descriptions_repeated_len = registrations;

	memset(data_reg, 0, sizeof(struct DataDescription) * registrations);
}

size_t Packet_get_message_size(struct Packet *_Packet)
{
	size_t size = 0;

	if (_Packet->_data)
	{
		size += 1; // One byte for the field number and wire type.
		size += 1; // One byte for the length of Data, which is going to be max 40 or so.

		size += 1; // One byte for "handle" and wire type.
		size += 4; // 4 bytes for a varint to support a ton of handles.

		if (_Packet->_data->_type == VB_DATATYPE_INT)
		{
			size += 1; // One byte for the field number and wire type.
			size += 4; // 4 bytes for a varint.
		}

		if (_Packet->_data->_type == VB_DATATYPE_FLOAT)
		{
			size += 1; // One byte for the field number and wire type.
			size += 4; // 4 bytes for a float.
		}

		if (_Packet->_data->_type == VB_DATATYPE_VECTOR)
		{
			size += 1; // One byte for the field number and wire type.
			size += 4; // 4 bytes for a float.
			size += 4; // 4 bytes for a float.
			size += 4; // 4 bytes for a float.
		}
	}

	if (_Packet->_data_descriptions_repeated_len)
	{
		int i;

		size += 1; // One byte for the field number and wire type.
		size += 1; // One byte for the length of DataDescription, which is going to be max 50 or so.

		size += 1; // One byte for "type" field number and wire type.
		size += 2; // Two bytes in case we ever get a lot of types.

		size += 1; // One byte for "handle" field number and wire type.
		size += 4; // 4 bytes to support a ton of handles.

		size += 1; // One byte for "name" field number and wire type.
		size += 4; // 4 bytes to support really long strings.

		// Add on the size for each string.
		for (i = 0; i < _Packet->_data_descriptions_repeated_len; i++)
			size += _Packet->_data_descriptions[i]._field_name_len;
	}

	return size;
}

size_t Packet_serialize(struct Packet *_Packet, void *_buffer, size_t length)
{
	size_t end_offset = Packet_write(_Packet, _buffer, 0);

	VBAssert(end_offset < length);

	// Uh-oh, some overwriting happened. Too late to fix it, but don't use it.
	if (end_offset >= length)
		return 0;

	return end_offset;
}