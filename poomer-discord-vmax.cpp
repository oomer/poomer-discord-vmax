// poomer-discord-vmax.cpp - A Discord bot that processes VoxelMax (.vmax.zip) files and renders them with Bella
// 
// This program is a Discord bot that watches for .vmax.zip file uploads and 
// converts them to rendered images/animations using the Bella 3D rendering engine.

#include "../DPP/include/dpp/dpp.h" // Discord bot library - provides all Discord API functionality
#include <iostream> // Standard input/output - for console logging (std::cout, std::cerr)
#include <string> // String handling - for std::string class and std::to_string() function
#include <fstream> // File stream operations - for writing downloaded files to disk
#include <thread> // Threading support - for std::thread to send images asynchronously
#include <mutex> // Mutex support - for thread synchronization
#include <atomic> // Atomic support - for thread-safe boolean flags
#include <sqlite3.h> // SQLite database for FIFO queue
#include <condition_variable> // For efficient worker thread waiting
#include <termios.h> // Terminal I/O control - for hiding password input in getHiddenInput()
#include <unistd.h> // Unix standard definitions - provides STDIN_FILENO constant
#include <ctime> // Time functions - for std::time() to generate timestamps
#include <cstdlib> // C standard library - for setenv function
#include <cstdio> // C standard I/O - for snprintf function
#include <vector> // Dynamic arrays - for std::vector to hold image byte data
#include <chrono> // Time utilities - for std::chrono::seconds() delays
#include <algorithm> // Algorithm functions - for std::transform (string case conversion)
#include <tuple> // Tuple support - for std::tuple to hold job data with multiple fields
#include <cstring> // C string functions - for strcmp in database migration
#include <locale> // Locale support - for fixing locale issues with Bella Engine
#include <filesystem> // For file system operations
#include <cstdint> // For fixed-size integer types
#include <cmath> // For mathematical functions
#include <map> // For key-value pair data structures
#include <variant> // For material properties
#include <limits> // For std::numeric_limits

// Bella Engine SDK - for rendering and scene creation
#include "../bella_engine_sdk/src/bella_sdk/bella_scene.h" // For creating and manipulating 3D scenes in Bella
#include "../bella_engine_sdk/src/bella_sdk/bella_engine.h" // For Engine class and rendering functionality
#include "../bella_engine_sdk/src/dl_core/dl_main.inl" // Core functionality from the Diffuse Logic engine

#ifdef _WIN32
#include <windows.h> // For ShellExecuteW
#include <shellapi.h> // For ShellExecuteW
#include <codecvt> // For wstring_convert
#elif defined(__APPLE__) || defined(__linux__)
#include <sys/wait.h> // For waitpid
#endif

// oomer's helper utility code
#include "../oom/oom_license.h"
#include "../oom/oom_bella_engine.h" 
#include "../oom/oom_bella_misc.h"
#include "../oom/oom_bella_long.h"
#include "../oom/oom_bella_premade.h"
#include "../oom/oom_misc.h"         // common misc code
#include "../oom/oom_voxel_vmax.h"   // common vmax voxel code and structures
#include "../oom/oom_voxel_ogt.h"    // common opengametools voxel conversion wrappers

#define OGT_VOX_IMPLEMENTATION
#include "../opengametools/src/ogt_vox.h"

//==============================================================================
// FORWARD DECLARATIONS
//==============================================================================

dl::bella_sdk::Node add_ogt_mesh_to_scene(  dl::String bellaName, 
                                            ogt_mesh* ogtMesh, 
                                            dl::bella_sdk::Scene& belScene, 
                                            dl::bella_sdk::Node& belWorld );

dl::bella_sdk::Node addModelToScene(dl::Args& args, 
                                    dl::bella_sdk::Scene& belScene, 
                                    dl::bella_sdk::Node& belWorld, 
                                    const oom::vmax::Model& vmaxModel, 
                                    const std::vector<oom::vmax::RGBA>& vmaxPalette, 
                                    const std::array<oom::vmax::Material, 8>& vmaxMaterial); 

//==============================================================================
// WORK QUEUE CLASSES
//==============================================================================

/**
 * Structure representing a work item in the processing queue
 */
struct WorkItem {
    int64_t id;                    // Unique database ID
    std::string attachment_url;     // Discord attachment URL to download
    std::string original_filename;  // Original filename from Discord
    uint64_t channel_id;           // Discord channel ID for response
    uint64_t user_id;              // Discord user ID for mentions
    std::string username;          // Discord username for display
    std::string message_content;   // Discord message content for orbit parsing
    int64_t created_at;            // Unix timestamp when job was created
    int retry_count;               // Number of times this job has been retried
    
    WorkItem() : id(0), channel_id(0), user_id(0), created_at(0), retry_count(0) {}
};

/**
 * SQLite-backed FIFO work queue for managing .vmax.zip file processing jobs
 * Provides persistence across system crashes and sequential processing
 */
class WorkQueue {
private:
    sqlite3* db;
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> cancel_current_job{false};
    std::atomic<int64_t> current_job_id{0};
    
public:
    WorkQueue() : db(nullptr) {}
    
    ~WorkQueue() {
        if (db) {
            sqlite3_close(db);
        }
    }
    
    bool initialize(const std::string& db_path = "work_queue_vmax.db") {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to open SQLite database: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        const char* create_table_sql = R"(
            CREATE TABLE IF NOT EXISTS work_queue (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                attachment_url TEXT NOT NULL,
                original_filename TEXT NOT NULL,
                channel_id INTEGER NOT NULL,
                user_id INTEGER NOT NULL,
                created_at INTEGER NOT NULL,
                retry_count INTEGER DEFAULT 0,
                status TEXT DEFAULT 'pending',
                bella_start_time INTEGER DEFAULT 0,
                bella_end_time INTEGER DEFAULT 0,
                username TEXT DEFAULT '',
                message_content TEXT DEFAULT ''
            );
            
            CREATE INDEX IF NOT EXISTS idx_status_created 
            ON work_queue(status, created_at);
        )";
        
        char* error_msg = nullptr;
        rc = sqlite3_exec(db, create_table_sql, nullptr, nullptr, &error_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to create work queue table: " << error_msg << std::endl;
            sqlite3_free(error_msg);
            return false;
        }
        
        std::cout << "✅ Work queue database initialized: " << db_path << std::endl;
        
        // Clean up old completed jobs (older than 24 hours)
        const char* cleanup_sql = "DELETE FROM work_queue WHERE status = 'completed' AND bella_end_time < ?;";
        sqlite3_stmt* cleanup_stmt;
        rc = sqlite3_prepare_v2(db, cleanup_sql, -1, &cleanup_stmt, nullptr);
        if (rc == SQLITE_OK) {
            int64_t cutoff_time = std::time(nullptr) - (24 * 60 * 60); // 24 hours ago
            sqlite3_bind_int64(cleanup_stmt, 1, cutoff_time);
            rc = sqlite3_step(cleanup_stmt);
            int cleaned_count = sqlite3_changes(db);
            sqlite3_finalize(cleanup_stmt);
            if (cleaned_count > 0) {
                std::cout << "🧹 Cleaned up " << cleaned_count << " old completed job(s)" << std::endl;
            }
        }
        
        // Reset any stuck 'processing' jobs back to 'pending' on startup (but only ones without bella_end_time)
        const char* reset_processing_sql = "UPDATE work_queue SET status = 'pending' WHERE status = 'processing' AND bella_end_time = 0;";
        rc = sqlite3_exec(db, reset_processing_sql, nullptr, nullptr, &error_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to reset stuck processing jobs: " << error_msg << std::endl;
            sqlite3_free(error_msg);
        } else {
            int reset_count = sqlite3_changes(db);
            if (reset_count > 0) {
                std::cout << "🔄 Reset " << reset_count << " stuck processing job(s) back to pending" << std::endl;
            }
        }
        
        return true;
    }
    
    bool enqueue(const WorkItem& item) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* insert_sql = R"(
            INSERT INTO work_queue 
            (attachment_url, original_filename, channel_id, user_id, username, message_content, created_at, retry_count)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare insert statement: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_text(stmt, 1, item.attachment_url.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, item.original_filename.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, item.channel_id);
        sqlite3_bind_int64(stmt, 4, item.user_id);
        sqlite3_bind_text(stmt, 5, item.username.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, item.message_content.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 7, item.created_at);
        sqlite3_bind_int(stmt, 8, item.retry_count);
        
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to insert work item: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "📥 Enqueued job: " << item.original_filename << " (ID: " << sqlite3_last_insert_rowid(db) << ")" << std::endl;
        
        queue_condition.notify_one();
        return true;
    }
    
    bool dequeue(WorkItem& item) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        
        while (!shutdown_requested) {
            const char* select_sql = R"(
                SELECT id, attachment_url, original_filename, channel_id, user_id, username, message_content, created_at, retry_count
                FROM work_queue 
                WHERE status = 'pending'
                ORDER BY created_at ASC
                LIMIT 1;
            )";
            
            sqlite3_stmt* stmt;
            int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                std::cerr << "❌ Failed to prepare select statement: " << sqlite3_errmsg(db) << std::endl;
                return false;
            }
            
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                item.id = sqlite3_column_int64(stmt, 0);
                item.attachment_url = (const char*)sqlite3_column_text(stmt, 1);
                item.original_filename = (const char*)sqlite3_column_text(stmt, 2);
                item.channel_id = sqlite3_column_int64(stmt, 3);
                item.user_id = sqlite3_column_int64(stmt, 4);
                item.username = (const char*)sqlite3_column_text(stmt, 5);
                item.message_content = (const char*)sqlite3_column_text(stmt, 6);
                item.created_at = sqlite3_column_int64(stmt, 7);
                item.retry_count = sqlite3_column_int(stmt, 8);
                
                sqlite3_finalize(stmt);
                markProcessing(item.id);
                
                std::cout << "📤 Dequeued job " << item.id << ": " << item.original_filename << std::endl;
                return true;
                
            } else if (rc == SQLITE_DONE) {
                sqlite3_finalize(stmt);
                queue_condition.wait(lock);
            } else {
                std::cerr << "❌ Failed to select work item: " << sqlite3_errmsg(db) << std::endl;
                sqlite3_finalize(stmt);
                return false;
            }
        }
        
        return false;
    }
    
    bool markCompleted(int64_t item_id) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* update_sql = "UPDATE work_queue SET status = 'completed', bella_end_time = ? WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare completion update: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_bind_int64(stmt, 2, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to mark work item as completed: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "✅ Completed job " << item_id << std::endl;
        return true;
    }
    
    bool markFailed(int64_t item_id, int max_retries = 3) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* select_sql = "SELECT retry_count FROM work_queue WHERE id = ?;";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare retry count select: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, item_id);
        rc = sqlite3_step(stmt);
        
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            std::cerr << "❌ Work item " << item_id << " not found for retry update" << std::endl;
            return false;
        }
        
        int current_retries = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        
        if (current_retries >= max_retries) {
            std::cout << "💀 Job " << item_id << " failed permanently after " << current_retries << " retries" << std::endl;
            const char* delete_sql = "DELETE FROM work_queue WHERE id = ?;";
            sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, item_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        } else {
            std::cout << "🔄 Job " << item_id << " failed, retry " << (current_retries + 1) << "/" << max_retries << std::endl;
            const char* update_sql = "UPDATE work_queue SET retry_count = ?, status = 'pending' WHERE id = ?;";
            sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
            sqlite3_bind_int(stmt, 1, current_retries + 1);
            sqlite3_bind_int64(stmt, 2, item_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            queue_condition.notify_one();
        }
        
        return true;
    }
    
    bool markBellaStarted(int64_t item_id) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* update_sql = "UPDATE work_queue SET bella_start_time = ? WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare bella start time update: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_bind_int64(stmt, 2, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
            std::cerr << "❌ Failed to update bella start time: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        
        std::cout << "⏱️ Marked bella start time for job " << item_id << std::endl;
        return true;
    }
    
    std::vector<std::tuple<std::string, std::string, int64_t, int64_t, int64_t>> getHistory(int limit = 10) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::vector<std::tuple<std::string, std::string, int64_t, int64_t, int64_t>> result;
        
        const char* history_sql = R"(
            SELECT original_filename, username, bella_start_time, bella_end_time, created_at
            FROM work_queue 
            WHERE status = 'completed' AND bella_start_time > 0 AND bella_end_time > 0
            ORDER BY bella_end_time DESC
            LIMIT ?;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, history_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare history query: " << sqlite3_errmsg(db) << std::endl;
            return result;
        }
        
        sqlite3_bind_int(stmt, 1, limit);
        
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string filename = (const char*)sqlite3_column_text(stmt, 0);
            std::string username = (const char*)sqlite3_column_text(stmt, 1);
            int64_t bella_start_time = sqlite3_column_int64(stmt, 2);
            int64_t bella_end_time = sqlite3_column_int64(stmt, 3);
            int64_t created_at = sqlite3_column_int64(stmt, 4);
            result.emplace_back(filename, username, bella_start_time, bella_end_time, created_at);
        }
        
        sqlite3_finalize(stmt);
        return result;
    }
    
    std::string cancelCurrentJob() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* select_sql = R"(
            SELECT id, original_filename FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC
            LIMIT 1;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "❌ Failed to prepare current job select: " << sqlite3_errmsg(db) << std::endl;
            return "";
        }
        
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int64_t job_id = sqlite3_column_int64(stmt, 0);
            std::string filename = (const char*)sqlite3_column_text(stmt, 1);
            sqlite3_finalize(stmt);
            
            cancel_current_job = true;
            std::cout << "🛑 Admin requested cancellation of job " << job_id << ": " << filename << std::endl;
            return filename;
        } else {
            sqlite3_finalize(stmt);
        }
        
        return "";
    }
    
    bool shouldCancelCurrentJob() {
        return cancel_current_job.load();
    }
    
    void markCurrentJobCancelled() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        cancel_current_job = false;
        
        int64_t job_id = current_job_id.load();
        if (job_id > 0) {
            const char* delete_sql = "DELETE FROM work_queue WHERE id = ?;";
            sqlite3_stmt* stmt;
            int rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, job_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
                std::cout << "🗑️ Cancelled job " << job_id << " removed from database" << std::endl;
            }
            current_job_id = 0;
        }
    }
    
    void setCurrentJobId(int64_t job_id) {
        current_job_id = job_id;
    }
    
    uint64_t getCurrentJobOwnerId() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        
        const char* select_sql = R"(
            SELECT user_id FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC
            LIMIT 1;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return 0;
        }
        
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            uint64_t user_id = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            return user_id;
        }
        
        sqlite3_finalize(stmt);
        return 0;
    }
    
    void requestShutdown() {
        shutdown_requested = true;
        queue_condition.notify_all();
    }
    
    std::vector<std::tuple<std::string, std::string, bool, int64_t>> getQueueDisplay() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::vector<std::tuple<std::string, std::string, bool, int64_t>> result;
        
        const char* processing_sql = R"(
            SELECT original_filename, username, bella_start_time
            FROM work_queue 
            WHERE status = 'processing'
            ORDER BY created_at ASC;
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, processing_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string filename = (const char*)sqlite3_column_text(stmt, 0);
                std::string username = (const char*)sqlite3_column_text(stmt, 1);
                int64_t bella_start_time = sqlite3_column_int64(stmt, 2);
                result.emplace_back(filename, username, true, bella_start_time);
            }
            sqlite3_finalize(stmt);
        }
        
        const char* pending_sql = R"(
            SELECT original_filename, username
            FROM work_queue 
            WHERE status = 'pending'
            ORDER BY created_at ASC;
        )";
        
        rc = sqlite3_prepare_v2(db, pending_sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string filename = (const char*)sqlite3_column_text(stmt, 0);
                std::string username = (const char*)sqlite3_column_text(stmt, 1);
                result.emplace_back(filename, username, false, 0);
            }
            sqlite3_finalize(stmt);
        }
        
        return result;
    }
    
private:
    bool markProcessing(int64_t item_id) {
        const char* update_sql = "UPDATE work_queue SET status = 'processing' WHERE id = ?;";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }
        
        sqlite3_bind_int64(stmt, 1, item_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        return rc == SQLITE_DONE;
    }
};

//==============================================================================
// UTILITY FUNCTIONS
//==============================================================================

/**
 * Function to securely input text without displaying it on screen
 */
std::string getHiddenInput(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();
    
    termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    std::string input;
    std::getline(std::cin, input);
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;
    
    return input;
}

/**
 * Function to parse orbit from Discord message content
 */
int parseOrbit(const std::string& message_content) {
    std::string content_lower = message_content;
    std::transform(content_lower.begin(), content_lower.end(), content_lower.begin(), ::tolower);
    
    size_t pos = content_lower.find("orbit=");
    if (pos == std::string::npos) {
        return 0;
    }
    
    size_t start = pos + 6; // Length of "orbit="
    if (start >= message_content.length()) {
        return 0;
    }
    
    size_t end = message_content.find_first_of(" \t\n\r", start);
    if (end == std::string::npos) {
        end = message_content.length();
    }
    
    std::string frames_str = message_content.substr(start, end - start);
    
    try {
        int frames = std::stoi(frames_str);
        
        if (frames <= 0 || frames > 300) {
            std::cout << "⚠️ Invalid orbit frame count: " << frames << " (must be 1-300)" << std::endl;
            return 0;
        }
        
        std::cout << "✅ Parsed orbit frames: " << frames << std::endl;
        return frames;
        
    } catch (const std::exception& e) {
        std::cout << "⚠️ Failed to parse orbit frame count: " << e.what() << std::endl;
        return 0;
    }
}

//==============================================================================
// VMAX PROCESSING FUNCTIONS
//==============================================================================

/**
 * Function to process .vmax.zip file and convert to rendered output
 */
std::string processVmaxFile(dl::bella_sdk::Engine& engine, const std::vector<uint8_t>& vmax_data, const std::string& filename, const std::string& message_content, WorkQueue* work_queue, int64_t item_id) {
    std::cout << "🔄 Processing .vmax.zip file (" << vmax_data.size() << " bytes)..." << std::endl;
    
    // Fix locale issues for Bella Engine
    try {
        std::locale::global(std::locale("C"));
        std::cout << "✅ Set locale to 'C' for Bella Engine compatibility" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "⚠️ Could not set locale: " << e.what() << std::endl;
    }
    
    // Save .vmax.zip data to temporary file with unique name based on job ID
    std::string temp_vmax_filename = "temp_vmax_file_job" + std::to_string(item_id) + ".vmax.zip";
    std::ofstream temp_vmax_file(temp_vmax_filename, std::ios::binary);
    temp_vmax_file.write(reinterpret_cast<const char*>(vmax_data.data()), vmax_data.size());
    temp_vmax_file.close();
    
    std::cout << "💾 Saved .vmax.zip to working file: " << temp_vmax_filename << std::endl;
    
    // Extract the zip file to unique work directories based on job ID
    std::string work_dir = "voxel_max_workdir_job" + std::to_string(item_id);
    std::string temp_extract_dir = "temp_vmax_extract_job" + std::to_string(item_id);
    
    // First extract to temp directory
    std::string unzip_cmd = "unzip -o -d " + temp_extract_dir + " " + temp_vmax_filename;
    std::cout << "📦 Extracting: " << unzip_cmd << std::endl;
    
    int unzip_result = system(unzip_cmd.c_str());
    if (unzip_result != 0) {
        std::cout << "❌ Failed to extract .vmax.zip file" << std::endl;
        std::remove(temp_vmax_filename.c_str());
        return "";
    }
    
    // Find the .vmax directory and move its contents to work directory
    std::filesystem::remove_all(work_dir); // Clean any existing work directory
    std::filesystem::create_directories(work_dir);
    
    // Look for .vmax directory in extracted files
    bool found_vmax_dir = false;
    for (const auto& entry : std::filesystem::directory_iterator(temp_extract_dir)) {
        if (entry.is_directory() && entry.path().extension() == ".vmax") {
            std::cout << "📁 Found .vmax directory: " << entry.path().filename() << std::endl;
            
            // Copy contents to work directory
            for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path())) {
                if (file.is_regular_file()) {
                    auto rel_path = std::filesystem::relative(file.path(), entry.path());
                    auto dest_path = work_dir + "/" + rel_path.string();
                    
                    // Create parent directories if needed
                    std::filesystem::create_directories(std::filesystem::path(dest_path).parent_path());
                    std::filesystem::copy_file(file.path(), dest_path, std::filesystem::copy_options::overwrite_existing);
                }
            }
            found_vmax_dir = true;
            break;
        }
    }
    
    // Clean up temp extraction directory
    std::filesystem::remove_all(temp_extract_dir);
    
    if (!found_vmax_dir) {
        std::cout << "❌ No .vmax directory found in extracted files" << std::endl;
        std::remove(temp_vmax_filename.c_str());
        std::filesystem::remove_all(work_dir);
        return "";
    }
    
    try {
        // Get Bella scene (already initialized)
        auto belScene = engine.scene();
        
        // CRITICAL FIX: Clear all removable nodes from previous jobs to avoid scene contamination
        std::cout << "🧹 Clearing previous scene nodes for job " << item_id << "..." << std::endl;
        dl::UInt clearedCount = belScene.clearNodes(false); // Clear all nodes (not just unreferenced)
        std::cout << "✅ Cleared " << clearedCount << " nodes from previous jobs" << std::endl;
        
        // Initialize basic scene elements
        oom::bella::defaultScene2025(belScene);
        auto [belWorld, belMeshVoxel, belLiqVoxel, belVoxel, belEmitterBlockXform] = oom::bella::defaultSceneVoxel(belScene);
        
        // Extract base filename for output
        std::string base_filename = filename;
        if (base_filename.length() >= 9 && base_filename.substr(base_filename.length() - 9) == ".vmax.zip") {
            base_filename = base_filename.substr(0, base_filename.length() - 9);
        }
        
        std::cout << "📷 Setting output filename to: " << base_filename << ".jpg" << std::endl;
        
        belScene.beautyPass()["outputExt"] = ".jpg";
        belScene.beautyPass()["outputName"] = base_filename.c_str();
        auto imgOutputPath = belScene.createNode("outputImagePath", "vmaxOutputPath");
        imgOutputPath["ext"] = ".jpg";
        imgOutputPath["dir"] = ".";
        belScene.beautyPass()["saveImage"] = dl::Int(1);  // ENABLE image saving!
        belScene.beautyPass()["overridePath"] = imgOutputPath;

        // Create dummy args for the vmax processing
        dl::Args args(0, nullptr);
        
        // Process the VMAX scene
        dl::String vmaxDirName = work_dir.c_str();
        
        std::cout << "🎯 Processing VMAX scene from: " << vmaxDirName.buf() << std::endl;
        
        // Parse scene.json
        oom::vmax::JsonSceneParser vmaxSceneParser;
        std::string scene_json_path = work_dir + "/scene.json";
        
        if (!std::filesystem::exists(scene_json_path)) {
            std::cout << "❌ scene.json not found in extracted files" << std::endl;
            return "";
        }
        
        vmaxSceneParser.parseScene(scene_json_path.c_str());
        
        std::map<std::string, oom::vmax::JsonGroupInfo> jsonGroups = vmaxSceneParser.getGroups();
        std::map<dl::String, dl::bella_sdk::Node> belGroupNodes;
        std::map<dl::String, dl::bella_sdk::Node> belCanonicalNodes;

        // Create Bella nodes for groups
        for (const auto& [groupName, groupInfo] : jsonGroups) { 
            dl::String belGroupUUID = dl::String(groupName.c_str());
            belGroupUUID = belGroupUUID.replace("-", "_");
            belGroupUUID = "_" + belGroupUUID;
            belGroupNodes[belGroupUUID] = belScene.createNode("xform", belGroupUUID, belGroupUUID);

            oom::vmax::Matrix4x4 objectMat4 = oom::vmax::combineTransforms(groupInfo.rotation[0], 
                                              groupInfo.rotation[1], 
                                              groupInfo.rotation[2], 
                                              groupInfo.rotation[3],
                                              groupInfo.position[0], 
                                              groupInfo.position[1], 
                                              groupInfo.position[2], 
                                              groupInfo.scale[0], 
                                              groupInfo.scale[1], 
                                              groupInfo.scale[2]);

            belGroupNodes[belGroupUUID]["steps"][0]["xform"] = dl::Mat4({
                objectMat4.m[0][0], objectMat4.m[0][1], objectMat4.m[0][2], objectMat4.m[0][3],
                objectMat4.m[1][0], objectMat4.m[1][1], objectMat4.m[1][2], objectMat4.m[1][3],
                objectMat4.m[2][0], objectMat4.m[2][1], objectMat4.m[2][2], objectMat4.m[2][3],
                objectMat4.m[3][0], objectMat4.m[3][1], objectMat4.m[3][2], objectMat4.m[3][3]
                });
        }

        // Parent the groups
        for (const auto& [groupName, groupInfo] : jsonGroups) { 
            dl::String belGroupUUID = dl::String(groupName.c_str());
            belGroupUUID = belGroupUUID.replace("-", "_");
            belGroupUUID = "_" + belGroupUUID;
            if (groupInfo.parentId == "") {
                belGroupNodes[belGroupUUID].parentTo(belWorld);
            } else {
                dl::String belPPPGroupUUID = dl::String(groupInfo.parentId.c_str());
                belPPPGroupUUID = belPPPGroupUUID.replace("-", "_");
                belPPPGroupUUID = "_" + belPPPGroupUUID;
                dl::bella_sdk::Node myParentGroup = belGroupNodes[belPPPGroupUUID];
                belGroupNodes[belGroupUUID].parentTo(myParentGroup);
            }
        }

        // Process models
        auto modelVmaxbMap = vmaxSceneParser.getModelContentVMaxbMap(); 
        std::vector<oom::vmax::Model> allModels;
        std::vector<std::vector<oom::vmax::RGBA>> vmaxPalettes;
        std::vector<std::array<oom::vmax::Material, 8>> vmaxMaterials;
        
        std::cout << "🎨 Processing " << modelVmaxbMap.size() << " unique models..." << std::endl;
        
        // Process each unique model
        for (const auto& [vmaxContentName, vmaxModelList] : modelVmaxbMap) { 
            // Check for cancellation
            if (work_queue && work_queue->shouldCancelCurrentJob()) {
                std::cout << "🛑 Cancelling vmax processing for job " << item_id << std::endl;
                work_queue->markCurrentJobCancelled();
                return "";
            }
            
            std::cout << "📦 Processing model: " << vmaxContentName << std::endl;
            oom::vmax::Model currentVmaxModel(vmaxContentName);
            const auto& jsonModelInfo = vmaxModelList.front();

            // Get file names
            dl::String materialName = vmaxDirName + "/" + jsonModelInfo.paletteFile.c_str();
            materialName = materialName.replace(".png", ".settings.vmaxpsb");

            // Get this model's colors from the paletteN.png 
            dl::String pngName = vmaxDirName + "/" + jsonModelInfo.paletteFile.c_str();
            vmaxPalettes.push_back(oom::vmax::read256x1PaletteFromPNG(pngName.buf()));
            if (vmaxPalettes.empty()) { 
                throw std::runtime_error(std::string("Failed to read palette from: ") + pngName.buf()); 
            }

            // Read contentsN.vmaxb plist file, lzfse compressed
            dl::String modelFileName = vmaxDirName + "/" + jsonModelInfo.dataFile.c_str();
            plist_t plist_model_root = oom::vmax::readPlist(modelFileName.buf(), true);

            plist_t plist_snapshots_array = plist_dict_get_item(plist_model_root, "snapshots");
            uint32_t snapshots_array_size = plist_array_get_size(plist_snapshots_array);

            // Process snapshots
            for (uint32_t i = 0; i < snapshots_array_size; i++) {
                plist_t plist_snapshot = plist_array_get_item(plist_snapshots_array, i);
                plist_t plist_chunk = oom::vmax::getNestedPlistNode(plist_snapshot, {"s", "id", "c"});
                plist_t plist_datastream = oom::vmax::getNestedPlistNode(plist_snapshot, {"s", "ds"});
                uint64_t chunkID;
                plist_get_uint_val(plist_chunk, &chunkID);
                oom::vmax::ChunkInfo chunkInfo = oom::vmax::vmaxChunkInfo(plist_snapshot);
                std::vector<oom::vmax::Voxel> xvoxels = oom::vmax::vmaxVoxelInfo(plist_datastream, chunkInfo.id, chunkInfo.mortoncode);

                for (const auto& voxel : xvoxels) {
                    currentVmaxModel.addVoxel(voxel.x, voxel.y, voxel.z, voxel.material, voxel.palette, chunkInfo.id, chunkInfo.mortoncode);
                }
            }
            allModels.push_back(currentVmaxModel);
            
            // Parse the materials stored in paletteN.settings.vmaxpsb    
            plist_t plist_material = oom::vmax::readPlist(materialName.buf(), false);
            std::array<oom::vmax::Material, 8> currentMaterials = oom::vmax::getMaterials(plist_material);
            vmaxMaterials.push_back(currentMaterials);
        }

        std::cout << "🏗️ Creating canonical models..." << std::endl;
        
        // Create canonical models
        int modelIndex = 0;
        for (const auto& eachModel : allModels) {
            // Check for cancellation
            if (work_queue && work_queue->shouldCancelCurrentJob()) {
                std::cout << "🛑 Cancelling vmax processing for job " << item_id << std::endl;
                work_queue->markCurrentJobCancelled();
                return "";
            }
            
            std::cout << "🎨 Model " << modelIndex << ": " << eachModel.vmaxbFileName << " (voxels: " << eachModel.getTotalVoxelCount() << ")" << std::endl;
            
            dl::bella_sdk::Node belModel = addModelToScene(args, belScene, belWorld, eachModel, vmaxPalettes[modelIndex], vmaxMaterials[modelIndex]);
            
            dl::String lllmodelName = dl::String(eachModel.vmaxbFileName.c_str());
            dl::String lllcanonicalName = lllmodelName.replace(".vmaxb", "");
            belCanonicalNodes[lllcanonicalName.buf()] = belModel;
            modelIndex++;
        }

        std::cout << "🎪 Creating instances..." << std::endl;
        
        // Create instances
        for (const auto& [vmaxContentName, vmaxModelList] : modelVmaxbMap) { 
            oom::vmax::Model currentVmaxModel(vmaxContentName);
            for(const auto& jsonModelInfo : vmaxModelList) {
                // Check for cancellation
                if (work_queue && work_queue->shouldCancelCurrentJob()) {
                    std::cout << "🛑 Cancelling vmax processing for job " << item_id << std::endl;
                    work_queue->markCurrentJobCancelled();
                    return "";
                }
                
                std::vector<double> position = jsonModelInfo.position;
                std::vector<double> rotation = jsonModelInfo.rotation;
                std::vector<double> scale = jsonModelInfo.scale;
                auto jsonParentId = jsonModelInfo.parentId;
                auto belParentId = dl::String(jsonParentId.c_str());
                dl::String belParentGroupUUID = belParentId.replace("-", "_");
                belParentGroupUUID = "_" + belParentGroupUUID;

                auto belObjectId = dl::String(jsonModelInfo.id.c_str());
                belObjectId = belObjectId.replace("-", "_");
                belObjectId = "_" + belObjectId;

                dl::String getCanonicalName = dl::String(jsonModelInfo.dataFile.c_str());
                dl::String canonicalName = getCanonicalName.replace(".vmaxb", "");
                auto belCanonicalNode = belCanonicalNodes[canonicalName.buf()];
                auto foofoo = belScene.findNode(canonicalName);

                oom::vmax::Matrix4x4 objectMat4 = oom::vmax::combineTransforms(rotation[0], rotation[1], rotation[2], rotation[3],
                                                                 position[0], position[1], position[2], 
                                                                 scale[0], scale[1], scale[2]);

                auto belNodeObjectInstance = belScene.createNode("xform", belObjectId, belObjectId);
                belNodeObjectInstance["steps"][0]["xform"] = dl::Mat4({
                    objectMat4.m[0][0], objectMat4.m[0][1], objectMat4.m[0][2], objectMat4.m[0][3],
                    objectMat4.m[1][0], objectMat4.m[1][1], objectMat4.m[1][2], objectMat4.m[1][3],
                    objectMat4.m[2][0], objectMat4.m[2][1], objectMat4.m[2][2], objectMat4.m[2][3],
                    objectMat4.m[3][0], objectMat4.m[3][1], objectMat4.m[3][2], objectMat4.m[3][3]
                    });

                if (jsonParentId == "") {
                    belNodeObjectInstance.parentTo(belScene.world());
                } else {
                    dl::bella_sdk::Node myParentGroup = belGroupNodes[belParentGroupUUID];
                    belNodeObjectInstance.parentTo(myParentGroup);
                }
                foofoo.parentTo(belNodeObjectInstance);
            }
        }

        // Position camera to view the entire scene
        std::cout << "📷 Setting up camera positioning..." << std::endl;
        
        // Calculate bounding box of all voxels in the scene
        std::cout << "📐 Calculating scene bounding box from voxels..." << std::endl;
        
        double min_x = std::numeric_limits<double>::max();
        double min_y = std::numeric_limits<double>::max();
        double min_z = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double max_y = std::numeric_limits<double>::lowest();
        double max_z = std::numeric_limits<double>::lowest();
        
        int total_voxel_count = 0;
        
        // Iterate through all models to find voxel extents
        for (const auto& model : allModels) {
            // Get all material/color combinations for this model
            const auto& usedMaterialsAndColors = model.getUsedMaterialsAndColors();
            
            for (const auto& [material, colorIDs] : usedMaterialsAndColors) {
                for (int colorID : colorIDs) {
                    // Get all voxels for this material/color combination
                    const std::vector<oom::vmax::Voxel>& voxels = model.getVoxels(material, colorID);
                    
                    for (const auto& voxel : voxels) {
                        // Update bounding box with voxel position
                        min_x = std::min(min_x, static_cast<double>(voxel.x));
                        min_y = std::min(min_y, static_cast<double>(voxel.y));
                        min_z = std::min(min_z, static_cast<double>(voxel.z));
                        max_x = std::max(max_x, static_cast<double>(voxel.x));
                        max_y = std::max(max_y, static_cast<double>(voxel.y));
                        max_z = std::max(max_z, static_cast<double>(voxel.z));
                        total_voxel_count++;
                    }
                }
            }
        }

        // Zoom extents bbox and radius calculation
        // Initialize bbox to "inverted infinity" so first point will always expand it
        dl::Aabb sceneBbox;
        sceneBbox.min = dl::Pos3::make(std::numeric_limits<double>::max(), 
                                       std::numeric_limits<double>::max(), 
                                       std::numeric_limits<double>::max());
        sceneBbox.max = dl::Pos3::make(std::numeric_limits<double>::lowest(), 
                                       std::numeric_limits<double>::lowest(), 
                                       std::numeric_limits<double>::lowest());
        int voxelCount = 0;

        //  
        auto worldPaths = belScene.world().paths(); 
        for ( auto eachPath : worldPaths ) // World Tree
        {
            auto eachLeaf = eachPath.leaf();  
            if ( !eachLeaf.isTypeOf( "instancer" ) )
                continue;            
            voxelCount++;

            auto instances = eachLeaf["steps"][0]["instances"].asBufferT<dl::Mat4f>(); // just need count
            for (dl::UInt i = 0; i < instances.count; ++i) {
                // Since we are dealing with 1x1x1 voxels
                // approximate by using center of voxel instance instead of 8 corners
                // for bbox calculation
                auto instanceXform = eachPath.transform(0.0,i); // use InstanceIdx for world space xform
                auto instancePos = dl::math::translation(instanceXform); // Extract translation directly
                
                // No branch - always expand (faster for large instance counts)
                if (instancePos.x < sceneBbox.min.x) sceneBbox.min.x = instancePos.x;
                if (instancePos.y < sceneBbox.min.y) sceneBbox.min.y = instancePos.y;
                if (instancePos.z < sceneBbox.min.z) sceneBbox.min.z = instancePos.z;
                if (instancePos.x > sceneBbox.max.x) sceneBbox.max.x = instancePos.x;
                if (instancePos.y > sceneBbox.max.y) sceneBbox.max.y = instancePos.y;
                if (instancePos.z > sceneBbox.max.z) sceneBbox.max.z = instancePos.z;
            }
        }

        auto center = ( sceneBbox.min.v3 + sceneBbox.max.v3 ) * 0.5;
        auto radius =dl::math::norm( sceneBbox.max - sceneBbox.min ) * 0.5;
        dl::bella_sdk::zoomExtents(belScene.cameraPath(), dl::Vec3{center.x, center.y, center.z}, radius);       
        std::cout << "✅ Camera positioning complete" << std::endl;

        auto belCamera = belScene.camera();

        // Orbit camera slightly for better view
        auto offset1 = dl::Vec2 {-45, 0.0};
        dl::bella_sdk::orbitCamera(engine.scene().cameraPath(), offset1);
        
        // Save .bsz file for debugging
        std::string bsz_filename = base_filename + "_debug.bsz";
        std::cout << "💾 Saving Bella scene file for debugging: " << bsz_filename << std::endl;
        try {
            belScene.write(bsz_filename.c_str());
            std::cout << "✅ Bella scene saved: " << bsz_filename << std::endl;
        } catch (const std::exception& e) {
            std::cout << "⚠️ Failed to save Bella scene file: " << e.what() << std::endl;
        }

        // Mark bella start time
        if (work_queue) {
            work_queue->markBellaStarted(item_id);
        }

        // Check for orbit animation
        int orbit_frames = parseOrbit(message_content);
        
        if (orbit_frames > 0) {
            // Orbit camera animation rendering
            std::cout << "🎨 Starting orbit animation with " << orbit_frames << " frames..." << std::endl;
            
            belCamera["resolution"] = dl::Vec2{320, 320};
            
            for (int i = 0; i < orbit_frames; i++) {
                // Check for cancellation before each frame
                if (work_queue && work_queue->shouldCancelCurrentJob()) {
                    std::cout << "🛑 Cancelling orbit render for job " << item_id << " at frame " << i << std::endl;
                    work_queue->markCurrentJobCancelled();
                    return "";
                }
                
                std::cout << "📹 Rendering frame " << (i + 1) << "/" << orbit_frames << std::endl;
                
                auto offset = dl::Vec2{i*0.05, 0.0};
                dl::bella_sdk::orbitCamera(engine.scene().cameraPath(), offset);
                auto belBeautyPass = belScene.beautyPass();
                belBeautyPass["outputName"] = dl::String::format("frame_%04d", i);
                
                engine.start();
                while(engine.rendering()) { 
                    if (work_queue && work_queue->shouldCancelCurrentJob()) {
                        std::cout << "🛑 Cancelling orbit render during frame " << (i + 1) << std::endl;
                        engine.stop();
                        work_queue->markCurrentJobCancelled();
                        return "";
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                
                std::cout << "✅ Frame " << (i + 1) << " completed" << std::endl;
            }
            
            std::cout << "🎬 All frames rendered, creating MP4 with ffmpeg..." << std::endl;
            
            // Create MP4 using ffmpeg
            std::string output_mp4 = base_filename + "_orbit.mp4";
            std::string ffmpegCmd = "ffmpeg -y -loglevel error -framerate 30 -i frame_%04d.jpg -c:v libx264 -pix_fmt yuv420p " + output_mp4;
            
            std::cout << "Executing FFmpeg command: " << ffmpegCmd << std::endl;
            int result = system(ffmpegCmd.c_str());
            
            if (result == 0) {
                std::cout << "✅ MP4 conversion successful: " << output_mp4 << std::endl;
                
                // Clean up individual frame files
                for (int i = 0; i < orbit_frames; i++) {
                    char frame_file[32];
                    snprintf(frame_file, sizeof(frame_file), "frame_%04d.jpg", i);
                    std::remove(frame_file);
                }
                std::cout << "🧹 Cleaned up individual frame files" << std::endl;
                
                // Clean up temporary files
                std::filesystem::remove_all(work_dir);
                std::remove(temp_vmax_filename.c_str());
                
                return output_mp4;
            } else {
                std::cout << "❌ FFmpeg conversion failed with error code: " << result << std::endl;
                return "";
            }
            
        } else {
            // Single frame rendering
            std::cout << "🎨 Starting single frame bella render..." << std::endl;
            
            engine.start();
            
            // Wait for rendering to complete, checking for cancellation
            bool was_cancelled = false;
            while(engine.rendering()) { 
                if (work_queue && work_queue->shouldCancelCurrentJob()) {
                    std::cout << "🛑 Cancelling bella render for job " << item_id << std::endl;
                    engine.stop();
                    was_cancelled = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            if (was_cancelled) {
                std::cout << "🛑 Bella render cancelled successfully" << std::endl;
                work_queue->markCurrentJobCancelled();
                return "";
            }
            
            std::cout << "✅ Single frame render completed!" << std::endl;
            
            // Clean up temporary files
            std::filesystem::remove_all(work_dir);
            std::remove(temp_vmax_filename.c_str());
            
            return base_filename + ".jpg";
        }
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Error processing .vmax.zip file: " << e.what() << std::endl;
        
        // Clean up temporary files
        std::filesystem::remove_all(work_dir);
        std::remove(temp_vmax_filename.c_str());
        
        return "";
    }
}

/**
 * Worker thread function that processes the work queue sequentially
 */
void workerThread(dpp::cluster* bot, WorkQueue* work_queue, dl::bella_sdk::Engine* engine) {
    std::cout << "🔧 Worker thread started" << std::endl;
    
    WorkItem item;
    while (work_queue->dequeue(item)) {
        std::cout << "\n--- PROCESSING VMAX FILE (Job " << item.id << ") ---" << std::endl;
        std::cout << "Downloading: " << item.original_filename << std::endl;
        std::cout << "From URL: " << item.attachment_url << std::endl;
        
        // Download the .vmax.zip file using DPP's HTTP client
        std::cout << "🌐 Starting .vmax.zip file download..." << std::endl;
        
        std::mutex download_mutex;
        std::condition_variable download_cv;
        bool download_complete = false;
        bool download_success = false;
        std::string download_data;
        
        bot->request(item.attachment_url, dpp::m_get, [&](const dpp::http_request_completion_t& response) {
            std::lock_guard<std::mutex> lock(download_mutex);
            
            if (response.status == 200) {
                std::cout << "✅ Downloaded .vmax.zip file (" << response.body.size() << " bytes)" << std::endl;
                
                // DEBUG: Add simple checksum to verify file content
                std::hash<std::string> hasher;
                size_t content_hash = hasher(response.body);
                std::cout << "🔍 File content hash: " << std::hex << content_hash << std::dec << std::endl;
                
                download_data = response.body;
                download_success = true;
            } else {
                std::cout << "❌ Failed to download .vmax.zip file. Status: " << response.status << std::endl;
                download_success = false;
            }
            
            download_complete = true;
            download_cv.notify_one();
        });
        
        // Wait for download to complete
        {
            std::unique_lock<std::mutex> lock(download_mutex);
            download_cv.wait(lock, [&]{ return download_complete; });
        }
        
        if (download_success) {
            work_queue->setCurrentJobId(item.id);
            
            std::vector<uint8_t> original_data(download_data.begin(), download_data.end());
            
            if (work_queue->shouldCancelCurrentJob()) {
                std::cout << "🛑 Job " << item.id << " cancelled before processing" << std::endl;
                work_queue->markCurrentJobCancelled();
                continue;
            }
            
            // Process the .vmax.zip file
            std::string output_filename = processVmaxFile(*engine, original_data, item.original_filename, item.message_content, work_queue, item.id);
            
            if (work_queue->shouldCancelCurrentJob() || output_filename.empty()) {
                std::cout << "🛑 Job " << item.id << " was cancelled or failed during processing" << std::endl;
                if (work_queue->shouldCancelCurrentJob()) {
                    work_queue->markCurrentJobCancelled();
                }
                continue;
            }
            
            // Read and send the output file
            std::vector<uint8_t> file_data;
            dpp::message msg(item.channel_id, "");
            
            std::ifstream output_file(output_filename, std::ios::binary);
            
            if (output_file.is_open()) {
                output_file.seekg(0, std::ios::end);
                size_t file_size = output_file.tellg();
                output_file.seekg(0, std::ios::beg);
                
                file_data.resize(file_size);
                output_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
                output_file.close();
                
                std::cout << "📁 Read output file: " << output_filename << " (" << file_data.size() << " bytes)" << std::endl;
                
                bool is_mp4 = (output_filename.length() >= 4 && output_filename.substr(output_filename.length() - 4) == ".mp4");
                
                if (is_mp4) {
                    msg.content = "🎬 Here's your VoxelMax orbit animation! <@" + std::to_string(item.user_id) + ">";
                } else {
                    msg.content = "🎨 Here's your rendered VoxelMax image! <@" + std::to_string(item.user_id) + ">";
                }
                msg.add_file(output_filename, std::string(file_data.begin(), file_data.end()));
            } else {
                std::cout << "❌ Could not read output file: " << output_filename << std::endl;
                msg.content = "❌ Rendering completed but could not read output file. <@" + std::to_string(item.user_id) + ">";
            }
            
            // Send the message
            std::mutex send_mutex;
            std::condition_variable send_cv;
            bool send_complete = false;
            bool send_success = false;
            
            bot->message_create(msg, [&](const dpp::confirmation_callback_t& callback) {
                std::lock_guard<std::mutex> lock(send_mutex);
                
                if (callback.is_error()) {
                    std::cout << "❌ Failed to send message: " << callback.get_error().message << std::endl;
                    send_success = false;
                } else {
                    if (file_data.empty()) {
                        std::cout << "✅ Successfully sent error message for " << item.original_filename << std::endl;
                    } else {
                        std::cout << "✅ Successfully sent " << output_filename << "!" << std::endl;
                    }
                    send_success = true;
                }
                
                send_complete = true;
                send_cv.notify_one();
            });
            
            // Wait for send to complete
            {
                std::unique_lock<std::mutex> lock(send_mutex);
                send_cv.wait(lock, [&]{ return send_complete; });
            }
            
            if (send_success && !file_data.empty()) {
                work_queue->markCompleted(item.id);
            } else {
                work_queue->markFailed(item.id);
            }
            
        } else {
            // Download failed
            bot->message_create(dpp::message(item.channel_id, "❌ Failed to download .vmax.zip file for processing."));
            work_queue->markFailed(item.id);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "🔧 Worker thread shutting down" << std::endl;
}

struct MyEngineObserver : public dl::bella_sdk::EngineObserver
{
public:
    // Called when a rendering pass starts
    void onStarted(dl::String pass) override
    {
        dl::logInfo("Started pass %s", pass.buf());
    }

    // Called to update the current status of rendering
    void onStatus(dl::String pass, dl::String status) override
    {
        dl::logInfo("%s [%s]", status.buf(), pass.buf());
    }

    // Called to update rendering progress (percentage, time remaining, etc)
    void onProgress(dl::String pass, dl::bella_sdk::Progress progress) override
    {
        dl::logInfo("%s [%s]", progress.toString().buf(), pass.buf());
    }

    void onImage(dl::String pass, dl::bella_sdk::Image image) override
    {
        dl::logInfo("We got an image %d x %d.", (int)image.width(), (int)image.height());
    }  

    // Called when an error occurs during rendering
    void onError(dl::String pass, dl::String msg) override
    {
        dl::logError("%s [%s]", msg.buf(), pass.buf());
    }

    // Called when a rendering pass completes
    void onStopped(dl::String pass) override
    {
        dl::logInfo("Stopped %s", pass.buf());
    }

    // Returns the current progress as a string
    std::string getProgress() const {
        std::string* currentProgress = progressPtr.load();
        if (currentProgress) {
            return *currentProgress;
        } else {
            return "";
        }
    }

    // Cleanup resources in destructor
    ~MyEngineObserver() {
        setString(nullptr);
    }
private:
    // Thread-safe pointer to current progress string
    std::atomic<std::string*> progressPtr{nullptr};

    // Helper function to safely update the progress string
    void setString(std::string* newStatus) {
        std::string* oldStatus = progressPtr.exchange(newStatus);
        delete oldStatus;  // Clean up old string if it exists
    }
};



//==============================================================================
// MAIN FUNCTION - Discord bot entry point
//==============================================================================

int DL_main(dl::Args& args) {
    // Fix locale issues early
    try {
        std::locale::global(std::locale("C"));
        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        std::cout << "✅ Set system locale to 'C' for compatibility" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "⚠️ Could not set initial locale: " << e.what() << std::endl;
    }

    // Setup Bella logging callbacks
    int s_oomBellaLogContext = 0; 
    dl::subscribeLog(&s_oomBellaLogContext, oom::bella::log);
    dl::flushStartupMessages();

    // Add command line arguments
    args.add("tp", "thirdparty",    "",   "prints third party licenses");
    args.add("li", "licenseinfo",   "",   "prints license info");
    args.add("t",  "token",         "",   "Discord bot token");

    if (args.helpRequested()) {
        std::cout << args.help("poomer-discord-vmax (C) 2025 Harvey Fong","", "1.0") << std::endl;
        return 0;
    }
    
    if (args.have("--licenseinfo")) {
        std::cout << "poomer-discord-vmax (C) 2025 Harvey Fong" << std::endl;
        std::cout << oom::license::printLicense() << std::endl;
        return 0;
    }
 
    if (args.have("--thirdparty")) {
        std::cout << oom::license::printBellaSDK() << "\n====\n" << std::endl;
        std::cout << oom::license::printLZFSE() << "\n====\n" << std::endl;
        std::cout << oom::license::printLibPlist() << "\n====\n" << std::endl;
        std::cout << oom::license::printOpenGameTools() << "\n====\n" << std::endl;
        return 0;
    }

    // Initialize Bella Engine
    std::cout << "=== Discord VoxelMax Bot Startup ===" << std::endl;
    std::cout << "🎨 Initializing Bella Engine..." << std::endl;
    
    dl::bella_sdk::Engine engine;
    engine.scene().loadDefs();
   
    


    //oom::bella::MyEngineObserver engineObserver;
    MyEngineObserver engineObserver;
    engine.subscribe(&engineObserver);
    
    std::cout << "✅ Bella Engine initialized" << std::endl;

    // Initialize work queue database
    std::cout << "🗄️ Initializing work queue database..." << std::endl;
    
    WorkQueue work_queue;
    if (!work_queue.initialize()) {
        std::cerr << "❌ Failed to initialize work queue database" << std::endl;
        return 1;
    }
    
    // Get Discord bot token
    std::string BOT_TOKEN;
    if (args.have("--token")) {
        BOT_TOKEN = args.value("--token").buf();
        std::cout << "✅ Using token from command line" << std::endl;
    } else {
        BOT_TOKEN = getHiddenInput("Enter Discord Bot Token: ");
    }
    
    if (BOT_TOKEN.empty()) {
        std::cerr << "Error: Bot token cannot be empty!" << std::endl;
        return 1;
    }

    // Create Discord bot instance
	dpp::cluster bot(BOT_TOKEN, dpp::i_default_intents | dpp::i_message_content);

    // Enable logging
    bot.on_log(dpp::utility::cout_logger());
    
    // Start worker thread
    std::cout << "🔧 Starting worker thread..." << std::endl;
    std::thread worker(workerThread, &bot, &work_queue, &engine);

    // Set up event handler for file uploads
    bot.on_message_create([&work_queue](const dpp::message_create_t& event) {
        
        if (event.msg.author.is_bot()) {
            return;
        }
        
        if (!event.msg.attachments.empty()) {
            std::cout << "\n=== FILE UPLOAD DETECTED ===" << std::endl;
            std::cout << "User: " << event.msg.author.username << std::endl;
            std::cout << "Channel ID: " << event.msg.channel_id << std::endl;
            std::cout << "Attachments: " << event.msg.attachments.size() << std::endl;
            
            bool found_vmax = false;
            std::vector<dpp::attachment> vmax_attachments;
            
            for (const auto& attachment : event.msg.attachments) {
                std::cout << "  - File: " << attachment.filename << std::endl;
                std::cout << "    Size: " << attachment.size << " bytes" << std::endl;
                std::cout << "    URL: " << attachment.url << std::endl;
                
                // DEBUG: Add simple hash of the URL to track if we're getting unique URLs
                std::hash<std::string> url_hasher;
                size_t url_hash = url_hasher(attachment.url);
                std::cout << "    🔍 URL hash: " << std::hex << url_hash << std::dec << std::endl;
                
                std::string filename_lower = attachment.filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
                
                if (filename_lower.length() >= 9 && filename_lower.substr(filename_lower.length() - 9) == ".vmax.zip") {
                    std::cout << "    ✅ VMAX.ZIP FILE DETECTED!" << std::endl;
                    found_vmax = true;
                    vmax_attachments.push_back(attachment);
                }
            }
            
            if (found_vmax) {
                std::cout << "\n🎯 ACTION: Enqueueing .vmax.zip files for processing" << std::endl;
                
                event.reply("🎮 VoxelMax file(s) detected! Adding to render queue...");
                
                for (const auto& vmax_attachment : vmax_attachments) {
                    WorkItem item;
                    item.attachment_url = vmax_attachment.url;
                    item.original_filename = vmax_attachment.filename;
                    item.channel_id = event.msg.channel_id;
                    item.user_id = event.msg.author.id;
                    item.username = event.msg.author.username;
                    item.message_content = event.msg.content;
                    item.created_at = std::time(nullptr);
                    item.retry_count = 0;
                    
                    if (work_queue.enqueue(item)) {
                        std::cout << "✅ Enqueued: " << vmax_attachment.filename << std::endl;
                    } else {
                        std::cout << "❌ Failed to enqueue: " << vmax_attachment.filename << std::endl;
                    }
                }
            }
            std::cout << "============================" << std::endl;
        }
    });

    // Set up slash command handler
    bot.on_slashcommand([&bot, &work_queue](const dpp::slashcommand_t& event) {
        static int command_counter = 0;
        int command_id = ++command_counter;
        
        std::cout << "\n=== COMMAND RECEIVED #" << command_id << " ===" << std::endl;
        std::cout << "Command: " << event.command.get_command_name() << std::endl;
        std::cout << "User: " << event.command.get_issuing_user().username << std::endl;
        
        if (event.command.get_command_name() == "help") {
            std::string help_message = "🎮 I am a VoxelMax render bot! Drop .vmax.zip files and I'll convert them to beautiful images!\n\n**Commands:**\n• Upload .vmax.zip files - I'll automatically render them\n• Add `orbit=30` to your message for animations\n• `/queue` - See current render queue\n• `/history` - View recently completed renders\n• `/remove` - Cancel current rendering job";
            
            event.reply(help_message);
            
        } else if (event.command.get_command_name() == "queue") {
            auto queue_jobs = work_queue.getQueueDisplay();
            
            if (queue_jobs.empty()) {
                event.reply("🎉 No queued jobs! Any .vmax.zip file you send will be processed immediately!");
            } else {
                std::string queue_message = "";
                size_t pending_position = 1;
                
                for (const auto& job : queue_jobs) {
                    const std::string& filename = std::get<0>(job);
                    const std::string& username = std::get<1>(job);
                    bool is_processing = std::get<2>(job);
                    int64_t bella_start_time = std::get<3>(job);
                    
                    if (is_processing) {
                        std::string render_time_text = "";
                        if (bella_start_time > 0) {
                            int64_t elapsed_seconds = std::time(nullptr) - bella_start_time;
                            int minutes = elapsed_seconds / 60;
                            int seconds = elapsed_seconds % 60;
                            
                            if (minutes > 0) {
                                render_time_text = " (" + std::to_string(minutes) + "m " + std::to_string(seconds) + "s)";
                            } else {
                                render_time_text = " (" + std::to_string(seconds) + "s)";
                            }
                        }
                        
                        queue_message += "**Rendering:** `" + filename + "` - " + username + render_time_text + "\n";
                    } else {
                        queue_message += std::to_string(pending_position) + ". `" + filename + "` - " + username + "\n";
                        pending_position++;
                    }
                }
                
                event.reply(queue_message);
            }
            
        } else if (event.command.get_command_name() == "history") {
            auto history_jobs = work_queue.getHistory(10);
            
            if (history_jobs.empty()) {
                event.reply("📜 No completed renders found in history.");
            } else {
                std::string history_message = "📜 **Recent Completed Renders:**\n\n";
                
                for (const auto& job : history_jobs) {
                    const std::string& filename = std::get<0>(job);
                    const std::string& username = std::get<1>(job);
                    int64_t bella_start_time = std::get<2>(job);
                    int64_t bella_end_time = std::get<3>(job);
                    
                    int64_t render_seconds = bella_end_time - bella_start_time;
                    std::string render_time_text;
                    
                    if (render_seconds > 0) {
                        int minutes = render_seconds / 60;
                        int seconds = render_seconds % 60;
                        
                        if (minutes > 0) {
                            render_time_text = " ⏱️ " + std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
                        } else {
                            render_time_text = " ⏱️ " + std::to_string(seconds) + "s";
                        }
                    } else {
                        render_time_text = " ⏱️ timing data incomplete";
                    }
                    
                    history_message += "`" + filename + "` - " + username + render_time_text + "\n";
                }
                
                event.reply(history_message);
            }
            
        } else if (event.command.get_command_name() == "remove") {
            const std::vector<uint64_t> ADMIN_USER_IDS = {
                780541438022254624ULL  // harvey
            };
            
            uint64_t requesting_user_id = event.command.get_issuing_user().id;
            bool is_admin = std::find(ADMIN_USER_IDS.begin(), ADMIN_USER_IDS.end(), requesting_user_id) != ADMIN_USER_IDS.end();
            
            bool is_job_owner = false;
            auto job_owner_id = work_queue.getCurrentJobOwnerId();
            if (job_owner_id == requesting_user_id) {
                is_job_owner = true;
            }
            
            if (!is_admin && !is_job_owner) {
                event.reply("🚫 Access denied. You can only cancel your own jobs (or be an admin).");
                return;
            }
            
            std::string cancelled_filename = work_queue.cancelCurrentJob();
            
            if (!cancelled_filename.empty()) {
                event.reply("🛑 **Cancelling VoxelMax render:** `" + cancelled_filename + "`");
            } else {
                event.reply("ℹ️ No job is currently being processed.");
            }
            
        } else {
            event.reply("⚠️ This command is no longer supported. Please use `/help`, `/queue`, `/history`, or `/remove`.");
        }
    });

    // Helper function to register all slash commands
    auto register_all_commands = [&bot]() {
        std::cout << "Registering commands..." << std::endl;
        
        bot.global_command_create(dpp::slashcommand("help", "Show information about VoxelMax rendering commands", bot.me.id));
        bot.global_command_create(dpp::slashcommand("queue", "Show current render queue status", bot.me.id));
        bot.global_command_create(dpp::slashcommand("history", "Show recently completed renders with timing", bot.me.id));
        bot.global_command_create(dpp::slashcommand("remove", "Cancel current processing job (admin or job owner)", bot.me.id));
    };

    // Set up bot ready event
    bot.on_ready([&bot, register_all_commands](const dpp::ready_t& event) {
	    if (dpp::run_once<struct register_bot_commands>()) {
	        std::cout << "VoxelMax Bot is ready!" << std::endl;
	        std::cout << "Bot user: " << bot.me.username << " (ID: " << bot.me.id << ")" << std::endl;
	        register_all_commands();
	    }
    });

    // Start the bot
    std::cout << "Starting VoxelMax bot event loop..." << std::endl;
    
    std::thread bot_thread([&bot]() {
        bot.start(dpp::st_wait);
    });
    
    bot_thread.join();
    
    std::cout << "Bot shutting down, stopping worker thread..." << std::endl;
    work_queue.requestShutdown();
    worker.join();
    
    return 0;
}

//==============================================================================
// VMAX MODEL PROCESSING FUNCTIONS
//==============================================================================

dl::bella_sdk::Node addModelToScene(dl::Args& args,
                                    dl::bella_sdk::Scene& belScene, 
                                    dl::bella_sdk::Node& belWorld, 
                                    const oom::vmax::Model& vmaxModel, 
                                    const std::vector<oom::vmax::RGBA>& vmaxPalette, 
                                    const std::array<oom::vmax::Material, 8>& vmaxMaterial) {
    // Create Bella scene nodes for each voxel
    int i = 0;
    dl::String modelName = dl::String(vmaxModel.vmaxbFileName.c_str());
    dl::String canonicalName = modelName.replace(".vmaxb", "");
    dl::bella_sdk::Node belCanonicalNode;
    {
        dl::bella_sdk::Scene::EventScope es(belScene);

        auto belVoxel = belScene.findNode("oomVoxel");
        auto belLiqVoxel = belScene.findNode("oomLiqVoxel");
        auto belMeshVoxel = belScene.findNode("oomMeshVoxel");
        auto belVoxelForm = belScene.findNode("oomEmitterBlockXform");
        auto belBevel = belScene.findNode("oomBevel");

        auto modelXform = belScene.createNode("xform", canonicalName, canonicalName);
        modelXform["steps"][0]["xform"] = dl::Mat4 {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        for (const auto& [material, colorID] : vmaxModel.getUsedMaterialsAndColors()) {
            for (int color : colorID) {

                auto thisname = canonicalName + dl::String("Material") + dl::String(material) + dl::String("Color") + dl::String(color);

                auto belMaterial  = belScene.createNode("quickMaterial",
                                canonicalName + dl::String("vmaxMat") + dl::String(material) + dl::String("Color") + dl::String(color));
                bool isMesh = false;
                bool isBox = true;

                if(material==7) {
                    belMaterial["type"] = "liquid";
                    belMaterial["liquidDepth"] = 300.0f;
                    belMaterial["liquidIor"] = 1.33f;
                    isMesh = true;
                    isBox = false;
                } else if(material==6 || vmaxPalette[color-1].a < 255) {
                    belMaterial["type"] = "glass";
                    belMaterial["roughness"] = vmaxMaterial[material].roughness * 100.0f;
                    belMaterial["glassDepth"] = 500.0f;
                } else if(vmaxMaterial[material].metalness > 0.1f) {
                    belMaterial["type"] = "metal";
                    belMaterial["roughness"] = vmaxMaterial[material].roughness * 100.0f;
                } else if(vmaxMaterial[material].transmission > 0.0f) {
                    belMaterial["type"] = "dielectric";
                    belMaterial["transmission"] = vmaxMaterial[material].transmission;
                } else if(vmaxMaterial[material].emission > 0.0f) {
                    belMaterial["type"] = "emitter";
                    belMaterial["emitterUnit"] = "radiance";
                    belMaterial["emitterEnergy"] = vmaxMaterial[material].emission*100.0f;
                } else if(vmaxMaterial[material].roughness > 0.8999f) {
                    belMaterial["type"] = "diffuse";
                } else {
                    belMaterial["type"] = "plastic";
                    belMaterial["roughness"] = vmaxMaterial[material].roughness * 100.0f;
                }

                if (args.have("bevel") && material != 7) {
                    belMaterial["bevel"] = belBevel;
                }
                if (args.have("mode") && args.value("mode") == "mesh" || args.value("mode") == "both") {
                    isMesh = true;
                    isBox = false;
                }

                // Convert 0-255 to 0-1 , remember to -1 color index becuase voxelmax needs 0 to indicate no voxel
                double bellaR = static_cast<double>(vmaxPalette[color-1].r)/255.0;
                double bellaG = static_cast<double>(vmaxPalette[color-1].g)/255.0;
                double bellaB = static_cast<double>(vmaxPalette[color-1].b)/255.0;
                double bellaA = static_cast<double>(vmaxPalette[color-1].a)/255.0;
                belMaterial["color"] = dl::Rgba{
                    oom::misc::srgbToLinear(bellaR), 
                    oom::misc::srgbToLinear(bellaG), 
                    oom::misc::srgbToLinear(bellaB), 
                    bellaA
                };

                // Get all voxels for this material/color combination
                const std::vector<oom::vmax::Voxel>& voxelsOfType = vmaxModel.getVoxels(material, color);
                int showchunk =0;

                if (isMesh) {
                    auto belMeshXform  = belScene.createNode("xform",
                        thisname+dl::String("Xform"));
                    belMeshXform.parentTo(modelXform);

                    // Convert voxels of a particular color to ogt_vox_model
                    ogt_vox_model* ogt_model = oom::ogt::convert_voxelsoftype_to_ogt_vox(voxelsOfType);
                    ogt_mesh_rgba* palette = new ogt_mesh_rgba[256];
                    for (int i = 0; i < 256; i++) {
                        palette[i] = ogt_mesh_rgba{vmaxPalette[i].r, vmaxPalette[i].g, vmaxPalette[i].b, vmaxPalette[i].a};
                    }
                    ogt_voxel_meshify_context ctx = {};

                    // Convert ogt voxels to mesh
                    ogt_mesh* mesh = ogt_mesh_from_paletted_voxels_simple(  &ctx,
                                                                            ogt_model->voxel_data, 
                                                                            ogt_model->size_x, 
                                                                            ogt_model->size_y, 
                                                                            ogt_model->size_z, 
                                                                            palette ); 
                        
                    if (voxelsOfType.size() > 0) {
                        auto belMesh = add_ogt_mesh_to_scene(   thisname,
                                                                mesh,
                                                                belScene,
                                                                belWorld
                                                            );
                        belMesh.parentTo(belMeshXform);
                        belMeshXform["material"] = belMaterial;
                    } else { 
                        std::cout << "skipping" << color << "\n";
                    }
                }
                if (isBox) {
                    auto belInstancer  = belScene.createNode("instancer",
                        thisname);
                    auto xformsArray = dl::ds::Vector<dl::Mat4f>();
                    belInstancer["steps"][0]["xform"] = dl::Mat4 {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                    belInstancer.parentTo(modelXform);

                    for (const auto& eachvoxel : voxelsOfType) {
                        xformsArray.push_back( dl::Mat4f{  1, 0, 0, 0, 
                                                        0, 1, 0, 0, 
                                                        0, 0, 1, 0, 
                                                        (static_cast<float>(eachvoxel.x))+0.5f,
                                                        (static_cast<float>(eachvoxel.y))+0.5f,
                                                        (static_cast<float>(eachvoxel.z))+0.5f, 1 });
                    };
                    belInstancer["steps"][0]["instances"] = xformsArray;
                    belInstancer["material"] = belMaterial;
                    if(material==7) {
                        belLiqVoxel.parentTo(belInstancer);
                    } else {
                        belMeshVoxel.parentTo(belInstancer);
                    }
                    if(vmaxMaterial[material].emission > 0.0f) {
                        belVoxelForm.parentTo(belInstancer);
                    }
                }
            }
        }
        return modelXform;
    }
    return dl::bella_sdk::Node();
}

dl::bella_sdk::Node add_ogt_mesh_to_scene(  dl::String name, 
                                            ogt_mesh* meshmesh, 
                                            dl::bella_sdk::Scene& belScene, 
                                            dl::bella_sdk::Node& belWorld ) {

    auto ogtMesh = belScene.createNode("mesh", name+"ogtmesh", name+"ogtmesh");
    ogtMesh["normals"] = "flat";
    // Add vertices and faces to the mesh
    dl::ds::Vector<dl::Pos3f> verticesArray;
    for (uint32_t i = 0; i < meshmesh->vertex_count; i++) {
        const auto& vertex = meshmesh->vertices[i];
        uint32_t xx = static_cast<uint32_t>(vertex.pos.x);
        uint32_t yy = static_cast<uint32_t>(vertex.pos.y);
        uint32_t zz = static_cast<uint32_t>(vertex.pos.z);
        verticesArray.push_back(dl::Pos3f{ static_cast<float>(xx), 
                                            static_cast<float>(yy), 
                                            static_cast<float>(zz) });

    }

    ogtMesh["steps"][0]["points"] = verticesArray;

    dl::ds::Vector<dl::Vec4u> facesArray;
    for (size_t i = 0; i < meshmesh->index_count; i+=3) {
        facesArray.push_back(dl::Vec4u{ static_cast<unsigned int>(meshmesh->indices[i]), 
                                        static_cast<unsigned int>(meshmesh->indices[i+1]), 
                                        static_cast<unsigned int>(meshmesh->indices[i+2]), 
                                        static_cast<unsigned int>(meshmesh->indices[i+2]) });
    }
    ogtMesh["polygons"] = facesArray;
    return ogtMesh;
}