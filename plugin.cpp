/*
 * FogLAMP "simplepython" filter plugin.
 *
 * Copyright (c) 2019 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <plugin_api.h>
#include <config_category.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string>
#include <iostream>
#include <filter_plugin.h>
#include <filter.h>
#include <reading_set.h>
#include <version.h>
#include "simple_python.h"

#define FILTER_NAME "simple-python"

const char *default_config = QUOTE({
	"plugin" : {
		"description" : "Simple Python filter plugin",
               	"type" : "string",
		"default" :  FILTER_NAME,
		"readonly": "true"
		},
	"enable": {
		"description": "A switch that can be used to enable or disable execution of the Simple Python filter.",
		"type": "boolean",
		"displayName": "Enabled",
		"default": "false"
		},
	"code": {
		"description": "Python code to execute",
		"type": "code",
		"displayName": "Python code",
		"default": "",
		"order" : "1"
		}
	});

using namespace std;
static void* libpython_handle = NULL;
static void createPythonReadingData(PyObject* localDictionary,
				    vector<Reading *>::iterator& elem);
static vector<Datapoint *>* getFilteredReadingData(PyObject* result);

/**
 * The Filter plugin interface
 */
extern "C" {

/**
 * The plugin information structure
 */
static PLUGIN_INFORMATION info = {
	FILTER_NAME,              // Name
	VERSION,                  // Version
	0,                        // Flags
	PLUGIN_TYPE_FILTER,       // Type
	"1.0.0",                  // Interface version
	default_config            // Default plugin configuration
};

/**
 * Return the information about this plugin
 */
PLUGIN_INFORMATION *plugin_info()
{
	return &info;
}

/**
 * Initialise the plugin, called to get the plugin handle and setup the
 * output handle that will be passed to the output stream. The output stream
 * is merely a function pointer that is called with the output handle and
 * the new set of readings generated by the plugin.
 *     (*output)(outHandle, readings);
 * Note that the plugin may not call the output stream if the result of
 * the filtering is that no readings are to be sent onwards in the chain.
 * This allows the plugin to discard data or to buffer it for aggregation
 * with data that follows in subsequent calls
 *
 * @param config	The configuration category for the filter
 * @param outHandle	A handle that will be passed to the output stream
 * @param output	The output stream (function pointer) to which data is passed
 * @return		An opaque handle that is used in all subsequent calls to the plugin
 */
PLUGIN_HANDLE plugin_init(ConfigCategory* config,
			  OUTPUT_HANDLE *outHandle,
			  OUTPUT_STREAM output)
{
	SimplePythonFilter* handle =
		new SimplePythonFilter(FILTER_NAME,
					*config,
					outHandle,
					output);

	if (config->itemExists("code"))
	{
		handle->m_code = config->getValue("code");
	}
	else
	{
		Logger::getLogger()->fatal("Filter %s (%s) is missing the 'code' "
					   "configuration item, aborting filter setup",
					   handle->getConfig().getName().c_str(),
					   FILTER_NAME);
		// This aborts filter pipeline setup
		delete handle;
		return NULL;
	}

	if (!Py_IsInitialized())
	{
#ifdef PLUGIN_PYTHON_SHARED_LIBRARY
		string openLibrary = TO_STRING(PLUGIN_PYTHON_SHARED_LIBRARY);
		if (!openLibrary.empty())
		{
			libpython_handle = dlopen(openLibrary.c_str(),
						  RTLD_LAZY | RTLD_GLOBAL);
			if (!libpython_handle)
			{
				Logger::getLogger()->fatal("Filter %s (%s) cannot pre-load "
							   "'%s' library, aborting filter setup",
							   handle->getConfig().getName().c_str(),
							   FILTER_NAME,
							   openLibrary.c_str());
				// This aborts filter pipeline setup
				delete handle;
				return NULL;
			}
			else
			{
				Logger::getLogger()->info("Pre-loading of library '%s' "
							  "is needed on this system",
							  openLibrary.c_str());
			}
		}
#endif
		Py_Initialize();
		PyEval_InitThreads(); // Initialize and acquire the global interpreter lock (GIL)
		PyThreadState* save = PyEval_SaveThread(); // release GIL
		handle->m_init = true;

		Logger::getLogger()->debug("Python interpteter is being initialised by "
					   "filter (%s), name %s",
					   FILTER_NAME,
					   config->getName().c_str());
	}

	return (PLUGIN_HANDLE)handle;
}

/**
 * Ingest a set of readings into the plugin for processing
 *
 * @param handle	The plugin handle returned from plugin_init
 * @param readingSet	The readings to process
 */
void plugin_ingest(PLUGIN_HANDLE *handle,
		   READINGSET *readingSet)
{
	SimplePythonFilter* filter = (SimplePythonFilter *)handle;
	bool enabled = false;
	string pythonCode;

	// Lock configuration items
	filter->lock();
	enabled = filter->isEnabled();
	pythonCode = filter->m_code;
	// Unlock configuration items
	filter->unlock();

	if (!enabled || !pythonCode.length())
	{
		// Current filter is not active: just pass the readings set
		filter->m_func(filter->m_data, readingSet);
		return;
	}

	PyGILState_STATE state = PyGILState_Ensure(); // acquire GIL
	PyObject* main = PyImport_AddModule("__main__");
	PyObject* globalDictionary = PyModule_GetDict(main);
	PyObject* userData = PyDict_New();

	// Create a global variable, dict, called "user_data"
	// Python code can access it via: global user_data
	PyDict_SetItemString(globalDictionary, "user_data", userData);

	// Just get all the readings in the readingset
	vector<Reading *>* readings = ((ReadingSet *)readingSet)->getAllReadingsPtr();

	// Iterate the input readings
	for (vector<Reading *>::iterator elem = readings->begin();
					 elem != readings->end(); )
	{
		PyObject* localDictionary = PyDict_New();

		// Create "reading" dict only in localDictionary
		createPythonReadingData(localDictionary, elem);

		// Run Python code (with statements separated by \n)
		PyObject* run = PyRun_String(("exec(" + pythonCode + ")").c_str(),
					     Py_file_input,
					     globalDictionary,
					     localDictionary);

		if (PyErr_Occurred())
		{
			filter->logErrorMessage();
			elem++;
		}
		else
		{
			// Get 'reading' value: borrowed reference from localDictionary
			PyObject* result = PyDict_GetItemString(localDictionary, "reading");

			// Create a Reading object from Python 'reading' dict
			std::vector<Datapoint *>* points = getFilteredReadingData(result);

			if (points && points->size())
			{
				// Remove current datapoints
				(*elem)->removeAllDatapoints();

				// Add new ones
				for (auto it = points->begin();
					  it != points->end();
					  ++it)
				{
					(*elem)->addDatapoint(*it);	
				}
				delete points;
				elem++;
			}
			else
			{
				// Remove current reading
				delete(*elem);
				elem = readings->erase(elem);
			}
		}

		Py_CLEAR(run);
		Py_CLEAR(localDictionary);
	}

	PyDict_DelItemString(globalDictionary, "user_data");

	PyGILState_Release(state);


	// Call asset tracker
	for (vector<Reading *>::const_iterator elem = readings->begin();
						      elem != readings->end();
						      ++elem)
	{
		AssetTracker::getAssetTracker()->addAssetTrackingTuple(filter->getConfig().getName(),
									(*elem)->getAssetName(),
									string("Filter"));
	}

	// Pass readingSet to the next filter
	filter->m_func(filter->m_data, readingSet);
}

/**
 * Call the shutdown method in the plugin
 */
void plugin_shutdown(PLUGIN_HANDLE *handle)
{
	SimplePythonFilter* filter = (SimplePythonFilter *)handle;

	PyGILState_STATE state = PyGILState_Ensure();

	// Cleanup Python 3.x
	if (filter->m_init)
	{
		filter->m_init = false;

		Py_Finalize();

		if (libpython_handle)
		{
			dlclose(libpython_handle);
		}
	}
	else
	{
		// Interpreter is still running, just release the GIL
		PyGILState_Release(state);
	}

	// Remove filter object	
	delete filter;
}

/**
 * Apply filter plugin reconfiguration
 *
 * @param    handle	The plugin handle returned from plugin_init
 * @param    newConfig	The new configuration to apply.
 */
void plugin_reconfigure(PLUGIN_HANDLE *handle, const string& newConfig)
{
	SimplePythonFilter* filter = (SimplePythonFilter *)handle;
	ConfigCategory category("new", newConfig);

	// Lock configuration items
	filter->lock();

	// Update Python code to execute
	if (category.itemExists("code"))
	{
		filter->m_code = category.getValue("code");
	}

	// Update the enable flag
	if (category.itemExists("enable"))
	{
		bool enabled = category.getValue("enable").compare("true") == 0 ||
				category.getValue("enable").compare("True") == 0;

		filter->setEnableFilter(enabled);
	}

	// Unlock configuration items
	filter->unlock();
}

// End of extern "C"
};

/**
 * Creates a Python dict from a single Reading object
 * and adds it to local dictionary with key "reading"
 *
 * @param localDictionary	Python local dictionary dict
 *				where to add the new "reading" dict
 * @param elem			Reading iterator object
 */
static void createPythonReadingData(PyObject* localDictionary,
				    vector<Reading *>::iterator& elem)
{
	// Datapoints to add to localDictionary
	PyObject* newDataPoints = PyDict_New();

	// Get all datapoints in the current reading
	std::vector<Datapoint *>& dataPoints = (*elem)->getReadingData();
	for (auto it = dataPoints.begin(); it != dataPoints.end(); ++it)
	{
		PyObject* value;
		DatapointValue::dataTagType dataType = (*it)->getData().getType();

		if (dataType == DatapointValue::dataTagType::T_INTEGER)
		{
			value = PyLong_FromLong((*it)->getData().toInt());
		}
		else if (dataType == DatapointValue::dataTagType::T_FLOAT)
		{
			value = PyFloat_FromDouble((*it)->getData().toDouble());
		}
		else
		{
			value = PyBytes_FromString((*it)->getData().toString().c_str());
		}

		// Add Datapoint: key and value
		PyObject* key = PyBytes_FromString((*it)->getName().c_str());
		PyDict_SetItem(newDataPoints, key, value);

		// Remove temp objects
		Py_CLEAR(key);
		Py_CLEAR(value);
	}

	// Add reading datapoints to localDictionary
	PyDict_SetItemString(localDictionary,
			     "reading",
			     newDataPoints);
}

/**
 * Get reading datapoints from a Python dict
 *
 * @param    result	PyObject with filtered reading datapoints
 * @return		Vector of Datapoint pointers or NULL
 */
static vector<Datapoint *>* getFilteredReadingData(PyObject* result)
{
	if (!result ||
	    !PyDict_Check(result) ||
	    !PyDict_Size(result))
	{
		return NULL;
	}

	// Allocate output result
	vector<Datapoint *>* newDatapoints = new vector<Datapoint *>();

	/*
	 * Create a Reading object from Python dict
	 *
	 * Fetch all Datapoins in 'reading' dict
	 * dKey and dValue are borrowed references
	 */
	PyObject *dKey, *dValue;
	Py_ssize_t dPos = 0;
	while (PyDict_Next(result, &dPos, &dKey, &dValue))
	{
		DatapointValue* dataPoint;
		if (PyLong_Check(dValue))
		{
			dataPoint =
				new DatapointValue((long)PyLong_AsUnsignedLongMask(dValue));
		}
		else if (PyFloat_Check(dValue))
		{
			dataPoint =
				new DatapointValue(PyFloat_AS_DOUBLE(dValue));
		}
		else if (PyBytes_Check(dValue))
		{
			dataPoint =
				new DatapointValue(string(PyBytes_AsString(dValue)));
		}
		else if (PyUnicode_Check(dValue))
		{
			dataPoint =
				new DatapointValue(string(PyUnicode_AsUTF8(dValue)));
		}
		else
		{
			delete dataPoint;

			break;
		}

		// Get datapointName:
		// reading[b'key'] ==> PyBytes_AsString(dKey)
		// reading['key'] ==> PyUnicode_AsUTF8(dKey)
		string datapointName = PyUnicode_Check(dKey) ?
					PyUnicode_AsUTF8(dKey) :
					PyBytes_AsString(dKey);

		// Add datapoint to the output vector
		newDatapoints->push_back(new Datapoint(datapointName, *dataPoint));

		// Remove temp object
		delete dataPoint;
	}

	// Return vector of datapoints
	return newDatapoints;
}

/**
 * Log current Python 3.x error message
 */
void SimplePythonFilter::logErrorMessage()
{
#ifdef PYTHON_CONSOLE_DEBUG
	// Print full Python stacktrace 
	PyErr_Print();
#endif
	//Get error message
	PyObject *pType, *pValue, *pTraceback;
	PyErr_Fetch(&pType, &pValue, &pTraceback);
	PyErr_NormalizeException(&pType, &pValue, &pTraceback);

	PyObject* str_exc_value = PyObject_Repr(pValue);
	PyObject* pyExcValueStr = PyUnicode_AsEncodedString(str_exc_value,
							    "utf-8",
							    "Error ~");

	// NOTE from :
	// https://docs.python.org/3.5/c-api/exceptions.html
	//
	// The value and traceback object may be NULL
	// even when the type object is not.	
	const char* pErrorMessage = pValue ?
				    PyBytes_AsString(pyExcValueStr) :
				    "no error description.";

	Logger::getLogger()->fatal("Filter '%s', Python code "
				   "'%s': Error '%s'",
				   this->getConfig().getName().c_str(), 
				   m_code.c_str(),
				   pErrorMessage);

	// Reset error
	PyErr_Clear();

	// Remove references
	Py_CLEAR(pType);
	Py_CLEAR(pValue);
	Py_CLEAR(pTraceback);
	Py_CLEAR(str_exc_value);
	Py_CLEAR(pyExcValueStr);
}
